/*
 * Player::audio_producer_loop — extracted verbatim from player.cpp
 * as part of `debt-split-player-cpp`. The pre-split TU was 706 lines
 * (forced P0 per SKILL §R.5: ≥700-line files). Carving the audio
 * producer thread body into its own TU drops player.cpp under the
 * threshold without churning behaviour.
 *
 * Scope: only `Player::audio_producer_loop()`. Member access stays
 * within the Player class definition declared in player.hpp;
 * rational helpers consumed via player_internal.hpp. No new state,
 * no new methods.
 *
 * Threading + lifetime contract is unchanged: the loop runs on the
 * audio producer thread spawned by `Player::play()`, exits cleanly
 * on `shutdown_`, and uses the same `mu_` / `state_cv_` mutex pair
 * as the video producer (declared in player.hpp).
 */
#include "orchestrator/player.hpp"
#include "orchestrator/player_internal.hpp"

#include "audio/tempo.hpp"
#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "orchestrator/audio_graph.hpp"
#include "scheduler/scheduler.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

using player_detail::r_add;
using player_detail::r_cmp;
using player_detail::r_micros_diff;

void Player::audio_producer_loop() {
    if (!has_audio_track_) return;
    const int sr       = cfg_.audio_out.sample_rate;
    const int channels = cfg_.audio_out.num_channels > 0
                             ? cfg_.audio_out.num_channels : 2;
    if (sr <= 0) return;
    if (!engine_ || !engine_->scheduler) return;

    /* Throttle: stay at most `audio_queue_ms` ahead of the master
     * clock. Default 200 ms when caller passed 0. */
    const int queue_ms = cfg_.audio_queue_ms > 0 ? cfg_.audio_queue_ms : 200;

    AudioChunkParams params;
    params.target_rate     = sr;
    params.target_channels = channels;
    params.target_fmt      = AV_SAMPLE_FMT_FLTP;

    /* Frame budget per chunk. A typical AAC frame at 48k carries 1024
     * samples; the kernel pipeline preserves that count through to
     * the mixer output. We use 1024 here purely for the cursor-
     * advance fallback when a graph eval returns ME_E_NOT_FOUND
     * (gap inside the timeline) — the actual chunk size comes from
     * each evaluated AVFrame's nb_samples. */
    const int gap_advance_samples = 1024;

    while (true) {
        /* Wait for: shutdown OR is_playing. While paused we sit here
         * until play() (or destroy) wakes the state_cv. */
        {
            std::unique_lock<std::mutex> lk(mu_);
            state_cv_.wait(lk, [this] {
                return shutdown_ || clock_.is_playing();
            });
            if (shutdown_) return;
        }

        /* Past timeline end → park until shutdown. */
        me_rational_t cursor;
        me_rational_t duration;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cursor   = audio_chunk_cursor_;
            duration = tl_ ? tl_->duration : me_rational_t{0, 1};
        }
        if (r_cmp(cursor, duration) >= 0) {
            std::unique_lock<std::mutex> lk(mu_);
            state_cv_.wait(lk, [this] { return shutdown_; });
            return;
        }

        /* Snapshot the current playback rate. set in play(); reset
         * to 1.0 by play(1.0) but NOT by pause() (pause leaves the
         * stretcher state intact for resume). */
        const float rate = current_rate_.load(std::memory_order_acquire);
        const bool  rate_eq_1 =
            !(rate < 0.999999f) && !(rate > 1.000001f);

        /* Throttle ahead of clock.
         *
         * Without tempo (rate == 1): audio_dispatched_samples_ counts
         * host samples emitted = timeline samples consumed (1:1), so
         * `dispatched_t` (samples / sr) is BOTH wall and timeline
         * time, and direct compare against clock_.current() is
         * already wall-time-correct.
         *
         * With tempo (rate != 1): audio_dispatched_samples_ still
         * counts HOST samples emitted, but each host sample now
         * represents `rate` timeline samples worth of source content
         * (rate=2 → output half as long → each host sample carries
         * 2 timeline samples). Compare both sides in TIMELINE units:
         *   dispatched_timeline = audio_dispatched_samples * rate / sr
         *   ahead_timeline_us   = dispatched_timeline - clock_.current()
         * The wall-time queue budget the user asked for (queue_ms)
         * is rate × that in timeline units (rate=2 → budget×2 timeline
         * because the queue plays out at 2× wall). Float math here is
         * deliberate: this is a flow-control heuristic with a 200 ms
         * slop, well beyond float-rate precision (§3a.2 carve-out
         * for non-decision-logic time conversion at the boundary). */
        int64_t ahead_us = 0;
        const me_rational_t now = clock_.current();
        if (rate_eq_1) {
            const me_rational_t dispatched_t{
                audio_dispatched_samples_,
                static_cast<int64_t>(sr)
            };
            ahead_us = r_micros_diff(dispatched_t, now);
        } else {
            const double dispatched_s_timeline =
                static_cast<double>(audio_dispatched_samples_) *
                static_cast<double>(rate) / static_cast<double>(sr);
            const double now_s = (now.den != 0)
                ? static_cast<double>(now.num) /
                  static_cast<double>(now.den)
                : 0.0;
            ahead_us = static_cast<int64_t>(
                (dispatched_s_timeline - now_s) * 1e6);
        }
        const int64_t budget_us =
            rate_eq_1
                ? static_cast<int64_t>(queue_ms) * 1000
                : static_cast<int64_t>(
                      static_cast<double>(queue_ms) * 1000.0 *
                      static_cast<double>(rate));
        if (ahead_us > budget_us) {
            std::unique_lock<std::mutex> lk(mu_);
            if (shutdown_) return;
            const auto budget = std::chrono::microseconds(
                std::min<int64_t>(ahead_us - budget_us, 5'000));
            state_cv_.wait_for(lk, budget);
            if (shutdown_) return;
            continue;
        }

        /* Compile + evaluate the per-chunk audio graph. ME_E_NOT_FOUND
         * means no audio clip covers the cursor — advance one frame's
         * worth and retry, same shape as the video producer's gap
         * handling. */
        graph::Graph   g;
        graph::PortRef term{};
        const me_status_t cs = compile_audio_chunk_graph(
            *tl_, cursor, params, &g, &term);
        if (cs == ME_E_NOT_FOUND) {
            std::lock_guard<std::mutex> lk(mu_);
            audio_chunk_cursor_ = r_add(audio_chunk_cursor_,
                me_rational_t{gap_advance_samples, static_cast<int64_t>(sr)});
            continue;
        }
        if (cs != ME_OK) {
            /* Malformed timeline / kernel error — backoff and keep
             * the loop alive. */
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        graph::EvalContext ctx;
        ctx.frames = engine_->frames.get();
        ctx.codecs = engine_->codecs.get();
        ctx.time   = cursor;
        ctx.engine = engine_;

        std::shared_ptr<AVFrame> frame;
        try {
            auto fut = engine_->scheduler->evaluate_port<std::shared_ptr<AVFrame>>(
                g, term, ctx);
            frame = fut.await();
        } catch (const std::exception& ex) {
            if (engine_) me::detail::set_error(engine_, std::string{ex.what()});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!frame || frame->nb_samples <= 0) {
            /* Empty chunk (e.g. SoundTouch latency at startup if a
             * future tempo path is wired). Advance by the standard
             * gap step and keep the loop alive. */
            std::lock_guard<std::mutex> lk(mu_);
            audio_chunk_cursor_ = r_add(audio_chunk_cursor_,
                me_rational_t{gap_advance_samples, static_cast<int64_t>(sr)});
            continue;
        }

        const int n_ch  = frame->ch_layout.nb_channels;
        const int n_smp = frame->nb_samples;

        me_player_audio_cb cb;
        void*              user;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb   = audio_cb_;
            user = audio_user_;
        }

        if (rate_eq_1) {
            /* Direct emit — FLTP planes from the AVFrame map straight
             * to the host cb's `const float* const*` shape. */
            const me_rational_t chunk_start{
                audio_dispatched_samples_,
                static_cast<int64_t>(sr)
            };
            if (cb) {
                cb(reinterpret_cast<const float* const*>(frame->data),
                   n_ch, n_smp, chunk_start, user);
            }
            std::lock_guard<std::mutex> lk(mu_);
            audio_dispatched_samples_ += n_smp;
            audio_chunk_cursor_ = r_add(audio_chunk_cursor_,
                me_rational_t{n_smp, static_cast<int64_t>(sr)});
        } else {
            /* Tempo-stretch path. Lazy-create the stretcher; reuse
             * across iterations to keep its internal FFT scratch +
             * FIFO state. set_tempo on every iter is cheap (just
             * writes a parameter) so re-applying the snapshot value
             * is fine even when unchanged. */
            if (!tempo_) {
                tempo_ = std::make_unique<me::audio::TempoStretcher>(
                    sr, n_ch);
            }
            tempo_->set_tempo(static_cast<double>(rate));

            /* Interleave FLTP → contiguous float buffer for SoundTouch.
             * AVFrame::data[ch] is the per-channel plane (n_smp floats).
             * SoundTouch wants L,R,L,R,... interleaved. */
            std::vector<float> interleaved(
                static_cast<std::size_t>(n_smp) * n_ch);
            for (int ch = 0; ch < n_ch; ++ch) {
                const float* plane =
                    reinterpret_cast<const float*>(frame->data[ch]);
                for (int i = 0; i < n_smp; ++i) {
                    interleaved[static_cast<std::size_t>(i) * n_ch + ch] =
                        plane[i];
                }
            }
            tempo_->put_samples(interleaved.data(),
                                static_cast<std::size_t>(n_smp));

            /* Drain whatever SoundTouch produces this iteration into
             * one or more host cb chunks. Cap chunk size at n_smp
             * (1 input frame's worth) so the host cb's input shape
             * stays predictable; we'll loop until receive_samples
             * returns 0. n_out per call is bounded by SoundTouch's
             * internal buffer state. */
            const std::size_t cap_frames = static_cast<std::size_t>(n_smp);
            std::vector<float> out_inter(cap_frames * n_ch);
            std::vector<std::vector<float>> out_planar(
                static_cast<std::size_t>(n_ch),
                std::vector<float>(cap_frames));
            std::vector<const float*> plane_ptrs(
                static_cast<std::size_t>(n_ch));

            while (true) {
                const std::size_t got = tempo_->receive_samples(
                    out_inter.data(), cap_frames);
                if (got == 0) break;

                /* De-interleave back to planar for the host cb. */
                for (int ch = 0; ch < n_ch; ++ch) {
                    for (std::size_t i = 0; i < got; ++i) {
                        out_planar[ch][i] =
                            out_inter[i * n_ch + ch];
                    }
                    plane_ptrs[ch] = out_planar[ch].data();
                }

                /* chunk_start in TIMELINE time = position of the
                 * source content this output represents. With
                 * SoundTouch's internal lookahead, "where in the
                 * input did this output start" is approximately
                 * (audio_dispatched_samples * rate / sr) at the
                 * moment of emit — we account for the stretcher's
                 * latency by computing chunk_start AFTER
                 * audio_dispatched_samples updated for prior emits.
                 * This is the same approximation the existing
                 * rate=1 path uses (chunk_start = dispatched_t
                 * before increment); good enough for the host cb's
                 * UI-display purposes. Float→int rounding is at the
                 * µs level, well below frame-period resolution. */
                int64_t cur_dispatched;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    cur_dispatched = audio_dispatched_samples_;
                }
                const double cs_s_timeline =
                    static_cast<double>(cur_dispatched) *
                    static_cast<double>(rate) /
                    static_cast<double>(sr);
                /* Express as rational in microseconds to keep the
                 * cb's me_rational_t signature without re-deriving
                 * from float. 1µs precision is fine; any
                 * sub-microsecond drift is below host audio
                 * scheduler resolution. */
                const me_rational_t chunk_start_t{
                    static_cast<int64_t>(cs_s_timeline * 1'000'000.0),
                    1'000'000
                };

                if (cb) {
                    cb(plane_ptrs.data(), n_ch,
                       static_cast<int>(got),
                       chunk_start_t, user);
                }

                std::lock_guard<std::mutex> lk(mu_);
                audio_dispatched_samples_ +=
                    static_cast<int64_t>(got);
            }

            /* Always advance the timeline read cursor by the input
             * we consumed this iter, regardless of whether
             * SoundTouch emitted anything (its lookahead may swallow
             * a chunk before producing output — that's fine; the
             * next iter feeds more input and the buffered output
             * surfaces). */
            std::lock_guard<std::mutex> lk(mu_);
            audio_chunk_cursor_ = r_add(audio_chunk_cursor_,
                me_rational_t{n_smp, static_cast<int64_t>(sr)});
        }
    }
}

}  // namespace me::orchestrator
