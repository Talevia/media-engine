/*
 * PlaybackClock — master clock abstraction for the Player orchestrator.
 *
 * Holds (timeline_anchor, wall_anchor, rate, paused) and answers
 * `current()` in timeline coordinates. AUDIO master clock arrives in
 * a follow-up phase: hosts will then call `report_audio_playhead`
 * which overrides the WALL projection until the next pause/seek.
 *
 * Threading: all public methods are mutex-guarded; safe to call from
 * any thread. `current()` is the hot path for the pacer.
 */
#pragma once

#include "media_engine/player.h"
#include "media_engine/types.h"

#include <chrono>
#include <mutex>

namespace me::orchestrator {

class PlaybackClock {
public:
    using SteadyClock = std::chrono::steady_clock;

    explicit PlaybackClock(me_master_clock_kind_t kind);

    /* Move the playhead to `t` and stop. Ring/queue invalidation is
     * the caller's job. */
    void seek(me_rational_t t);

    /* Resume from the current anchor at `rate`. */
    void play(float rate);

    /* Snapshot the playhead at "now" and stop the wall projection.
     * Subsequent `current()` returns the snapshot until play()/seek(). */
    void pause();

    /* Host-supplied audio playhead. Stored for the next `current()`
     * call when master == AUDIO; ignored otherwise (no-op). */
    void report_audio_playhead(me_rational_t t);

    /* Where is the playhead right now (timeline coordinates)?
     * - AUDIO: latest audio playhead if any has been reported, else
     *   WALL projection (cold-start fallback).
     * - WALL : timeline_anchor + (now - wall_anchor) * rate, frozen
     *   at pause(). */
    me_rational_t current() const;

    bool                   is_playing()    const;
    me_master_clock_kind_t kind()          const { return kind_; }

private:
    /* WALL projection at `now`, computed under lock. */
    me_rational_t wall_project_locked(SteadyClock::time_point now) const;

    mutable std::mutex          mu_;
    me_master_clock_kind_t      kind_;
    me_rational_t               anchor_t_{0, 1};   /* timeline t at wall_anchor_ */
    SteadyClock::time_point     wall_anchor_{};
    float                       rate_         = 0.0f;   /* 0 = paused */
    bool                        playing_      = false;

    /* AUDIO master: last reported audio playhead. has_audio_ph_ flips
     * true on first report; before that, AUDIO mode falls back to
     * WALL projection. */
    bool                        has_audio_ph_ = false;
    me_rational_t               audio_ph_{0, 1};
};

}  // namespace me::orchestrator
