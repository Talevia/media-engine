/*
 * Player::pacer_loop — extracted from player.cpp as part of
 * `debt-split-player-cpp-423`. The pre-extract TU was 423 lines
 * (above SKILL §R.5's 400-line P1 threshold; held ctor/dtor +
 * producer_loop + pacer_loop after cycle 26's transport extraction
 * dropped it from 565 → 423). Carving the smaller of the two thread
 * bodies (pacer_loop ~140 lines) into its own TU drops player.cpp
 * comfortably under the threshold without churning behaviour.
 *
 * Scope: only `Player::pacer_loop()`. Member access stays within
 * the Player class definition declared in player.hpp; rational
 * helpers consumed via player_internal.hpp (specifically
 * `r_micros_diff` for the chase-clock comparison).
 *
 * Threading + lifetime contract is unchanged: the loop runs on the
 * pacer thread spawned by `Player::play()` (via the ctor's thread
 * spawn), exits cleanly on `shutdown_` or `ring_.close()`, and uses
 * the same `mu_` / `state_cv_` mutex pair as the producer +
 * audio_producer threads (declared in player.hpp).
 *
 * Mirrors the player_audio_producer.cpp + player_transport.cpp
 * extractions (cycles 21 + 26).
 */
#include "orchestrator/player.hpp"
#include "orchestrator/player_internal.hpp"

#include "core/frame_impl.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace me::orchestrator {

using player_detail::r_micros_diff;

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
