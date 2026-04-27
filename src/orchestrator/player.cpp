#include "orchestrator/player.hpp"
#include "orchestrator/player_internal.hpp"

#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "io/ffmpeg_raii.hpp"
#include "orchestrator/audio_graph.hpp"
#include "orchestrator/compose_frame.hpp"
#include "scheduler/scheduler.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace me::orchestrator {

/* Rational helpers (r_add / r_cmp / r_micros_diff /
 * frame_period_from_rate) live in `player_internal.hpp` since
 * the `debt-split-player-cpp` cycle so player_audio_producer.cpp
 * (and any future player_*.cpp split TUs) can share the same
 * arithmetic without each repeating the bodies. */
using player_detail::r_add;
using player_detail::r_cmp;
using player_detail::r_micros_diff;
using player_detail::frame_period_from_rate;

/* ----------------------------------------------------------------- ctor */

Player::Player(me_engine*                            engine,
               std::shared_ptr<const me::Timeline>   timeline,
               const me_player_config_t&             cfg)
    : engine_(engine),
      tl_(std::move(timeline)),
      cfg_(cfg),
      clock_(cfg.master_clock == ME_CLOCK_AUTO
                 ? (tl_ && [&]{
                       for (const auto& tr : tl_->tracks) {
                           if (tr.kind == me::TrackKind::Audio) return true;
                       }
                       return false;
                   }() ? ME_CLOCK_AUDIO : ME_CLOCK_WALL)
                 : cfg.master_clock),
      ring_(cfg.video_ring_capacity > 0
                ? static_cast<std::size_t>(cfg.video_ring_capacity)
                : 3) {

    if (tl_) frame_period_ = frame_period_from_rate(tl_->frame_rate);

    /* The clock anchors at t=0 in paused state; play() bumps anchor. */
    clock_.seek(me_rational_t{0, 1});

    /* Audio path. Lit when audio_out.sample_rate > 0 AND the timeline
     * has at least one Audio track. Per-chunk graph evaluation —
     * compile_audio_chunk_graph + scheduler.evaluate_port — replaces
     * the streaming AudioMixer + per-asset DemuxContext fan-out the
     * Player held pre-kernel-ize. */
    if (cfg_.audio_out.sample_rate > 0 && tl_) {
        for (const auto& tr : tl_->tracks) {
            if (tr.kind == me::TrackKind::Audio) { has_audio_track_ = true; break; }
        }
        if (has_audio_track_) {
            audio_producer_ = std::thread(&Player::audio_producer_loop, this);
        }
    }

    producer_ = std::thread(&Player::producer_loop, this);
    pacer_    = std::thread(&Player::pacer_loop, this);
}

/* ----------------------------------------------------------------- dtor */

Player::~Player() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
        /* Cancel any in-flight video evaluation so the producer's
         * await unblocks immediately rather than waiting out a slow
         * decode. Same pattern as seek() — Future::cancel is the
         * cooperative signal; the kernel checks ctx.cancel and
         * returns early. */
        if (in_flight_video_) in_flight_video_->cancel();
    }
    state_cv_.notify_all();
    ring_.close();   /* unblocks producer (push) and pacer (pop) */
    if (producer_.joinable())       producer_.join();
    if (pacer_.joinable())          pacer_.join();
    if (audio_producer_.joinable()) audio_producer_.join();
}

/* `Player::play / pause / seek / report_audio_playhead /
 * current_time / is_playing / set_video_callback / set_audio_callback /
 * set_external_clock_callback / notify_state_changed_locked` all live
 * in `src/orchestrator/player_transport.cpp` since
 * `debt-split-player-cpp-560` (cycle 26). Class definition +
 * threading contract are unchanged; only the bodies moved.
 *
 * `Player::audio_producer_loop` lives in
 * `src/orchestrator/player_audio_producer.cpp` since cycle 21's
 * `debt-split-player-cpp`. */

/* ---------------------------------------------------- producer thread */

void Player::producer_loop() {
    while (true) {
        me_rational_t cursor;
        int64_t       epoch_start;
        {
            std::unique_lock<std::mutex> lk(mu_);
            /* Wait for: shutdown OR cursor inside the timeline (so we
             * have something to produce). When `produce_cursor_` is
             * past the timeline duration we sit here until a seek
             * brings it back. */
            state_cv_.wait(lk, [this] {
                if (shutdown_) return true;
                if (!tl_)      return false;
                /* Has clips left? We use simple cursor-vs-duration
                 * check; gaps inside the timeline are handled
                 * downstream by resolve_active_clip returning
                 * NOT_FOUND, in which case we advance + retry. */
                return r_cmp(produce_cursor_, tl_->duration) < 0;
            });
            if (shutdown_) return;
            cursor      = produce_cursor_;
            epoch_start = seek_epoch_;
        }

        /* Compile + evaluate. compile_compose_graph + the inline
         * scheduler submission below are both blocking; the slow
         * piece is the await on the decode + sws_scale graph. The
         * scheduler's OutputCache absorbs repeats.
         *
         * Player can't use the convenience compose_frame_at()
         * because that does submit+await as one shot — Player needs
         * to publish the in-flight Future for cooperative cancel on
         * seek. So it uses the lower-level primitives directly. */
        if (!tl_) {
            std::unique_lock<std::mutex> lk(mu_);
            if (shutdown_) return;
            state_cv_.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }

        /* Submit the per-frame graph. Stash a copy of the Future on
         * the player so seek() can cancel it; producer awaits its
         * own local copy. Both share the EvalInstance via shared_ptr
         * so cancel from either side reaches the running kernel.
         *
         * The Graph object MUST outlive the await — sched::EvalInstance
         * stores it by const reference (eval_instance.hpp:66), so a
         * graph that goes out of scope before await returns leaves
         * the EvalInstance reading garbage memory (kernels would see
         * TaskKindId 0 / "no kernel registered"). Keep `g` in this
         * outer scope so its lifetime spans the entire submit + await. */
        std::shared_ptr<me::graph::RgbaFrameData> rgba;
        VideoFuture          fut;
        me::graph::Graph     g;
        me::graph::PortRef   term{};
        bool                 submit_failed = false;
        bool                 gap = false;
        std::string          err;
        try {
            if (!engine_ || !engine_->scheduler) throw std::runtime_error("no scheduler");
            const me_status_t cs = compile_compose_graph(*tl_, cursor, &g, &term);
            if (cs == ME_E_NOT_FOUND) {
                gap = true;
            } else if (cs != ME_OK) {
                err = "compile_compose_graph failed";
                submit_failed = true;
            } else {
                me::graph::EvalContext ctx;
                ctx.frames = engine_->frames.get();
                ctx.codecs = engine_->codecs.get();
                ctx.time   = cursor;
                fut = engine_->scheduler->evaluate_port<
                           std::shared_ptr<me::graph::RgbaFrameData>>(g, term, ctx);
            }
        } catch (const std::exception& ex) {
            err = ex.what();
            submit_failed = true;
        }

        if (gap) {
            /* Timeline gap at this cursor — advance one frame and
             * retry. Same recovery as before, just relocated post-
             * compile (now that compile_compose_graph is the lookup
             * point). */
            std::lock_guard<std::mutex> lk(mu_);
            if (seek_epoch_ == epoch_start &&
                r_cmp(produce_cursor_, cursor) == 0) {
                produce_cursor_ = r_add(produce_cursor_, frame_period_);
            }
            continue;
        }

        if (!submit_failed) {
            bool epoch_skipped = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                /* If a seek raced past us between epoch capture and
                 * submit, the work is for an old cursor. Cancel +
                 * skip publishing — but still await below so the
                 * taskflow drains its tasks before `fut` (and the
                 * EvalInstance it owns by shared_ptr) goes out of
                 * scope. Skipping the await would let tasks run
                 * with a dangling EvalInstance pointer and crash
                 * inside the executor's worker thread. */
                if (seek_epoch_ != epoch_start) {
                    fut.cancel();
                    epoch_skipped = true;
                } else {
                    in_flight_video_ = fut;
                }
            }
            try {
                rgba = fut.await();
            } catch (const std::exception& ex) {
                /* Cancellation lands here as ME_E_CANCELLED-from-
                 * EvalInstance + thrown by Future::await; treat
                 * identically to other failures (drop + retry). */
                err = ex.what();
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                in_flight_video_.reset();
            }
            if (epoch_skipped) continue;
        }

        /* Re-check epoch. If seek bumped it during await, the rgba
         * we got is for a stale cursor — drop. */
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stale = (seek_epoch_ != epoch_start);
        }
        if (stale) continue;

        if (submit_failed || !rgba) {
            if (engine_ && !err.empty()) me::detail::set_error(engine_, std::move(err));
            std::lock_guard<std::mutex> lk(mu_);
            if (seek_epoch_ == epoch_start &&
                r_cmp(produce_cursor_, cursor) == 0) {
                produce_cursor_ = r_add(produce_cursor_, frame_period_);
            }
            continue;
        }

        VideoFrameSlot slot;
        slot.present_at = cursor;
        slot.rgba       = std::move(rgba);
        slot.seek_epoch = epoch_start;
        if (!ring_.push(std::move(slot))) {
            /* Ring closed — shutdown in flight. */
            return;
        }

        /* Advance only if no seek raced us. Otherwise the seek
         * already updated produce_cursor_ to the seek target. */
        std::lock_guard<std::mutex> lk(mu_);
        if (seek_epoch_ == epoch_start &&
            r_cmp(produce_cursor_, cursor) == 0) {
            produce_cursor_ = r_add(produce_cursor_, frame_period_);
        }
    }
}

/* ------------------------------------------------------ pacer thread */

void Player::pacer_loop() {
    while (true) {
        VideoFrameSlot slot;
        if (!ring_.pop(&slot)) {
            return;   /* closed */
        }

        /* Drop slots that came from a pre-seek epoch. Without this
         * check, a frame produced just before seek but pushed after
         * ring_.clear (the producer's stale-check + push aren't
         * atomic under mu_) would surface as a "ghost" frame at the
         * old cursor minutes after the seek — the pacer would wait
         * for the master clock to reach the old slot.present_at,
         * which never happens cleanly post-seek and confuses host
         * UI. */
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (slot.seek_epoch != seek_epoch_) {
                continue;
            }
        }

        /* Wait until the master clock reaches `slot.present_at`.
         * Polled in short intervals so a pause / seek wakes us via
         * state_cv_ within ≤ 5 ms. */
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (shutdown_) return;
                /* Re-check the epoch each loop turn — seek may have
                 * fired AFTER the initial epoch check at pop time
                 * but BEFORE this slot was actually presented. The
                 * slot's present_at is then for an old playhead the
                 * post-seek clock will never naturally reach, so we
                 * have to drop here rather than waiting forever. */
                if (slot.seek_epoch != seek_epoch_) {
                    break;
                }
            }

            const me_rational_t now = clock_.current();
            const int64_t diff_us   = r_micros_diff(slot.present_at, now);

            /* Chase-master-clock thresholds (Phase 4):
             *
             *   diff_us > +half_us     → frame is in the future, wait
             *   |diff_us| ≤ half_us    → present-now
             *   diff_us < -half_us     → past, drop (chase forward)
             *
             * `now` is whichever master clock the player was created
             * with: AUDIO when the host has been calling
             * me_player_report_audio_playhead (so video chases the
             * host's audio device), WALL otherwise (or AUDIO that
             * hasn't received a report yet — falls back through
             * PlaybackClock::current). The drop / wait branches
             * therefore apply to both modes uniformly; only the
             * source of the truth changes.
             *
             * frame_period stores {num, den} = num/den seconds, so
             * ½ frame_period in micros = num × 500'000 / den. (For
             * 30 fps: frame_period = {1, 30} → 16'666 us — sane.
             * Earlier code had num/den swapped which produced 15 s
             * thresholds and effectively disabled pacing.) */
            const int64_t half_us =
                frame_period_.den > 0
                    ? static_cast<int64_t>(frame_period_.num) * 500'000
                          / static_cast<int64_t>(frame_period_.den)
                    : 16'000;

            if (!clock_.is_playing()) {
                /* Park here until play()/seek() wakes us. The slot
                 * becomes stale across pauses; on resume we either
                 * present it (still close enough) or drop it (clock
                 * has jumped past via seek). Bare wait_for so any
                 * notify (play / pause / seek / shutdown) wakes the
                 * pacer immediately. */
                std::unique_lock<std::mutex> lk(mu_);
                if (shutdown_) return;
                state_cv_.wait_for(lk, std::chrono::milliseconds(20));
                if (shutdown_) return;
                continue;
            }

            if (diff_us > half_us) {
                /* Frame is too early. Sleep until close to present_at,
                 * but cap the wait so a pause / seek / new audio
                 * playhead report wakes us promptly via state_cv_.
                 * Cap is 10 ms — at AUDIO master clock that's also
                 * the typical inter-report interval from a host's
                 * audio device callback (~10 ms buffer durations are
                 * common), so we wake near every report. */
                std::unique_lock<std::mutex> lk(mu_);
                if (shutdown_) return;
                const auto budget = std::chrono::microseconds(
                    std::min<int64_t>(diff_us - half_us, 10'000));
                state_cv_.wait_for(lk, budget);
                if (shutdown_) return;
                continue;
            }
            if (diff_us < -half_us) {
                /* Stale — either produced before a seek that already
                 * advanced the clock past us, or pacer fell behind
                 * the audio playhead under decode pressure. Drop and
                 * pop the next slot. The producer will catch up if
                 * decode can keep pace with playback; otherwise the
                 * host sees frames stutter, which is the right
                 * signal that the timeline is too heavy for live
                 * preview. */
                break;
            }
            /* Within half-period of `now` — present. */

            me_player_video_cb cb;
            void*              user;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb   = video_cb_;
                user = video_user_;
            }
            if (cb && slot.rgba) {
                me_frame frame;
                frame.width  = slot.rgba->width;
                frame.height = slot.rgba->height;
                frame.stride = static_cast<int>(slot.rgba->stride);
                /* Borrow-vs-copy: me_frame's rgba is a std::vector,
                 * not a pointer, so we copy bytes for the duration
                 * of the callback. ~3.7 MB at 720p; acceptable for
                 * Phase 2. A future optimisation can introduce a
                 * borrowed-buffer me_frame variant if scrubbing
                 * pressure shows up. */
                frame.rgba = slot.rgba->rgba;
                cb(&frame, slot.present_at, user);
            }
            break;
        }
    }
}

}  // namespace me::orchestrator
