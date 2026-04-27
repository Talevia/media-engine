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

    /* Host-supplied external clock callback. Stored under mu_ but
     * INVOKED without the lock to keep host code off our critical
     * path (pacer calls current() at ~100 Hz, so a slow host
     * callback under-lock would serialise everything). cb=NULL
     * clears. Used only when kind_ == ME_CLOCK_EXTERNAL; for other
     * kinds the call is accepted but the stored callback is never
     * queried (matches report_audio_playhead's no-op-on-wrong-kind
     * shape). */
    void set_external_clock(me_player_external_clock_cb cb, void* user);

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

    /* EXTERNAL master: host callback. Both pointers default null —
     * before set_external_clock is called, kind_ == ME_CLOCK_EXTERNAL
     * falls back to WALL projection so a misconfigured host doesn't
     * deadlock the pacer. */
    me_player_external_clock_cb ext_cb_   = nullptr;
    void*                       ext_user_ = nullptr;
};

}  // namespace me::orchestrator
