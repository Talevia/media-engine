#include "orchestrator/playback_clock.hpp"

namespace me::orchestrator {

namespace {

/* a/b + (delta_seconds) * rate, rendered as a rational with
 * denominator = b * 1'000'000 (microsecond resolution suffices for
 * pacing — the pacer rounds frames to ½ frame_period anyway). */
me_rational_t add_seconds(me_rational_t base, double seconds) {
    if (seconds == 0.0) return base;
    if (base.den <= 0) base.den = 1;
    /* Convert seconds → micros for a stable integer denominator. */
    constexpr int64_t kUs = 1'000'000;
    const int64_t delta_us = static_cast<int64_t>(seconds * kUs);
    /* base + delta_us/kUs = (base.num * kUs + delta_us * base.den) / (base.den * kUs) */
    return me_rational_t{
        base.num * kUs + delta_us * base.den,
        base.den * kUs,
    };
}

}  // namespace

PlaybackClock::PlaybackClock(me_master_clock_kind_t kind) : kind_(kind) {}

void PlaybackClock::seek(me_rational_t t) {
    std::lock_guard<std::mutex> lk(mu_);
    if (t.den <= 0) t.den = 1;
    if (t.num < 0) t.num = 0;
    anchor_t_     = t;
    wall_anchor_  = SteadyClock::now();
    playing_      = false;
    rate_         = 0.0f;
    /* Drop the audio playhead — old reports are stale across a seek. */
    has_audio_ph_ = false;
}

void PlaybackClock::play(float rate) {
    std::lock_guard<std::mutex> lk(mu_);
    /* Re-anchor at the current playhead so the wall projection is
     * continuous across pause/play. */
    if (playing_) {
        anchor_t_ = wall_project_locked(SteadyClock::now());
    }
    wall_anchor_ = SteadyClock::now();
    rate_        = rate;
    playing_     = true;
}

void PlaybackClock::pause() {
    std::lock_guard<std::mutex> lk(mu_);
    if (playing_) {
        anchor_t_ = wall_project_locked(SteadyClock::now());
    }
    playing_ = false;
    rate_    = 0.0f;
}

void PlaybackClock::report_audio_playhead(me_rational_t t) {
    if (kind_ != ME_CLOCK_AUDIO) return;
    if (t.den <= 0) t.den = 1;
    if (t.num < 0) t.num = 0;
    std::lock_guard<std::mutex> lk(mu_);
    audio_ph_     = t;
    has_audio_ph_ = true;
}

void PlaybackClock::set_external_clock(me_player_external_clock_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(mu_);
    ext_cb_   = cb;
    ext_user_ = user;
}

me_rational_t PlaybackClock::current() const {
    /* EXTERNAL master: snapshot the callback pair under lock, then
     * release before invoking — host code runs without our mutex
     * held so a slow callback can't serialise other clock methods.
     * Cold-start (no callback yet) falls through to WALL so the
     * pacer doesn't sit on a dead clock. */
    if (kind_ == ME_CLOCK_EXTERNAL) {
        me_player_external_clock_cb cb;
        void*                       user;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb   = ext_cb_;
            user = ext_user_;
        }
        if (cb) {
            me_rational_t t = cb(user);
            if (t.den <= 0) t.den = 1;
            if (t.num <  0) t.num = 0;
            return t;
        }
        /* fall through to WALL */
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (kind_ == ME_CLOCK_AUDIO && has_audio_ph_) return audio_ph_;
    return wall_project_locked(SteadyClock::now());
}

bool PlaybackClock::is_playing() const {
    std::lock_guard<std::mutex> lk(mu_);
    return playing_;
}

me_rational_t PlaybackClock::wall_project_locked(SteadyClock::time_point now) const {
    if (!playing_) return anchor_t_;
    const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - wall_anchor_).count();
    const double secs = static_cast<double>(dt) / 1'000'000.0;
    return add_seconds(anchor_t_, secs * static_cast<double>(rate_));
}

}  // namespace me::orchestrator
