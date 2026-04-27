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
#include <exception>
#include <memory>
#include <string>

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

        /* Throttle ahead of clock. The next chunk we'd dispatch
         * starts at `audio_dispatched_samples_ / sr`. If that's more
         * than queue_ms ahead of the current playhead, sleep. */
        const me_rational_t dispatched_t{
            audio_dispatched_samples_,
            static_cast<int64_t>(sr)
        };
        const me_rational_t now = clock_.current();
        const int64_t       ahead_us = r_micros_diff(dispatched_t, now);
        if (ahead_us > static_cast<int64_t>(queue_ms) * 1000) {
            std::unique_lock<std::mutex> lk(mu_);
            if (shutdown_) return;
            const auto budget = std::chrono::microseconds(
                std::min<int64_t>(ahead_us - queue_ms * 1000, 5'000));
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
        const me_rational_t chunk_start = dispatched_t;

        me_player_audio_cb cb;
        void*              user;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb   = audio_cb_;
            user = audio_user_;
        }
        if (cb) {
            /* FLTP layout: AVFrame::data[ch] is the channel-`ch`
             * plane, sized n_smp × sizeof(float). Reinterpret the
             * uint8_t** as `const float* const*` for the cb. */
            cb(reinterpret_cast<const float* const*>(frame->data),
               n_ch, n_smp, chunk_start, user);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            audio_dispatched_samples_ += n_smp;
            audio_chunk_cursor_ = r_add(audio_chunk_cursor_,
                me_rational_t{n_smp, static_cast<int64_t>(sr)});
        }
    }
}

}  // namespace me::orchestrator
