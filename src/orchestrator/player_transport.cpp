/*
 * Player transport + introspection + callback registration —
 * extracted from player.cpp as part of `debt-split-player-cpp-560`.
 *
 * The pre-extract TU was 565 lines (above SKILL §R.5's 400-line
 * P1 threshold; the file re-grew after cycles 20/21 added the rate
 * gate + external-clock plumbing). Carving the C-API surface
 * methods (transport, introspection, callback setters, the
 * notify-state helper they share) into their own TU drops
 * player.cpp under the threshold without churning behaviour.
 *
 * Scope: Player::play / pause / seek / report_audio_playhead /
 * current_time / is_playing / set_video_callback / set_audio_callback /
 * set_external_clock_callback / notify_state_changed_locked.
 * Member access stays within the Player class definition declared
 * in player.hpp; rational helpers consumed via player_internal.hpp
 * (none used here, but the include stays for symmetry with
 * player_audio_producer.cpp).
 *
 * Threading + lifetime contract is unchanged: every method takes
 * the same `mu_` it always took (or no lock for the report-from-
 * audio-callback fast path), and the state_cv_ notify pattern is
 * identical.
 */
#include "orchestrator/player.hpp"

/* `scheduler.hpp` carries the inline definitions of
 * `Future<T>::await` / `Future<T>::cancel` (template methods, body
 * deferred to scheduler.hpp so EvalInstance's interface can be private
 * to src/scheduler). seek() uses cancel() on `in_flight_video_`, so
 * this TU needs the impl visible to instantiate the template. */
#include "scheduler/scheduler.hpp"

#include <cmath>
#include <mutex>

namespace me::orchestrator {

/* ---------------------------------------------------------- transport */

me_status_t Player::play(float rate) {
    /* Rate gating, in three tiers:
     *
     *   1. Hard ABI invariants — non-finite or non-positive rate is
     *      always wrong (zero is what pause() is for; negative is a
     *      reverse-playback follow-up that needs reverse demux + frame
     *      queue, tracked separately).
     *   2. Forward variable-rate window — 0.5..2.0 inclusive. Outside
     *      this window the master clock's projection still works (it's
     *      pure float math) but tonal artefacts and frame-drop ratios
     *      grow large enough that bound-by-design beats best-effort.
     *   3. Audio-bearing timelines — audio output is wired at the
     *      timeline's native sample rate; rate ≠ 1.0 desyncs against a
     *      host audio device unless we time-stretch via SoundTouch.
     *      That wiring is `debt-player-rate-audio-tempo` (this cycle's
     *      append). Until it lands, reject the combo so the host gets
     *      an explicit failure rather than silent A/V drift.
     *
     * The pacer (pacer_loop) and PlaybackClock (wall_project_locked)
     * already handle rate-aware projection — clock_.current()
     * advances at `rate ×` wall-clock seconds, and the pacer's
     * "diff_us > half_us → wait / diff_us < -half_us → drop" branches
     * naturally produce slow-motion (frame held longer) and fast-
     * forward (frames dropped) without explicit skip/repeat in the
     * producer. Only the play() admission gate needed to relax. */
    if (!std::isfinite(rate) || rate <= 0.0f) return ME_E_INVALID_ARG;
    /* LEGIT: outside [0.5, 2.0] forward window — bound-by-design per
     * tier (2) of the rate-gating doc above. Hosts asking for 4×
     * fast-forward / 0.25× slow-mo deserve a clear refusal. */
    if (rate < 0.5f || rate > 2.0f)            return ME_E_UNSUPPORTED;
    /* Audio + rate ≠ 1 used to be UNSUPPORTED here; now wired
     * through `me::audio::TempoStretcher` (SoundTouch) in
     * `audio_producer_loop`. The cursor accounting splits into
     * (a) input cursor advancing at the timeline sample rate, and
     * (b) dispatched cursor advancing at `n_smp / rate` host
     * samples per emit. See player_audio_producer.cpp for the
     * full read-stretch-emit loop. ME_CLOCK_AUDIO master + rate ≠ 1
     * also OK now: hosts that drive AUDIO master are responsible
     * for reporting the time-stretched playhead via
     * report_audio_playhead — without that the cold-start WALL
     * fallback in playback_clock.cpp keeps things sane. */
    current_rate_.store(rate, std::memory_order_release);
    clock_.play(rate);
    {
        std::lock_guard<std::mutex> lk(mu_);
        notify_state_changed_locked();
    }
    return ME_OK;
}

me_status_t Player::pause() {
    clock_.pause();
    {
        std::lock_guard<std::mutex> lk(mu_);
        notify_state_changed_locked();
    }
    return ME_OK;
}

me_status_t Player::seek(me_rational_t time) {
    if (time.den <= 0) time.den = 1;
    if (time.num <  0) time.num = 0;
    clock_.seek(time);

    /* Cooperative cancel of any in-flight video evaluation. Future::
     * cancel sets the EvalInstance's cancel flag; the active kernel
     * checks ctx.cancel between node boundaries and aborts, surfacing
     * as a thrown await(). Combined with seek_epoch_ bumping below,
     * this covers both the "decode running" case (cancel wakes await)
     * and the "between iterations" case (producer's epoch check at
     * the start of the next iteration drops stale work). */
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (in_flight_video_) in_flight_video_->cancel();
        ++seek_epoch_;
        produce_cursor_ = time;
        /* Reseat both audio cursors at the new playhead. The kernel-
         * graph audio path is stateless across chunks (each chunk's
         * graph evaluates IoDemux + IoDecodeAudio with the new
         * source_t in props), so no demux flush / mixer rebuild is
         * needed — the next compile_audio_chunk_graph call sees the
         * fresh time and emits the right samples. Pre-kernel-ize this
         * code held only audio_dispatched_samples_; now both cursors
         * track. */
        audio_chunk_cursor_ = time;
        const int sr = cfg_.audio_out.sample_rate;
        audio_dispatched_samples_ = sr > 0
            ? (time.num * static_cast<int64_t>(sr)) / time.den
            : 0;
        notify_state_changed_locked();
    }
    /* Drop the produced backlog — it's all stale relative to the new
     * playhead. Done after the lock release so producer threads
     * blocked on push() wake up without contending mu_ at the same
     * time as the cancel notify. */
    ring_.clear();
    return ME_OK;
}

me_status_t Player::report_audio_playhead(me_rational_t t) {
    clock_.report_audio_playhead(t);
    /* Wake the pacer + audio_producer so they re-evaluate against
     * the fresh playhead. notify_all without re-acquiring mu_ is
     * intentional: this method is meant to be called from the
     * host's real-time audio device callback, where blocking on
     * mu_ would jeopardise audio-output stability. CV semantics
     * tolerate the un-locked notify because the readers re-check
     * clock_.current() (which has its own lock inside
     * PlaybackClock) on every wake-up. */
    state_cv_.notify_all();
    return ME_OK;
}

me_rational_t Player::current_time() const { return clock_.current(); }
bool          Player::is_playing()   const { return clock_.is_playing(); }

/* --------------------------------------------------------- callback set */

me_status_t Player::set_video_callback(me_player_video_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(mu_);
    video_cb_   = cb;
    video_user_ = user;
    return ME_OK;
}

me_status_t Player::set_audio_callback(me_player_audio_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(mu_);
    audio_cb_   = cb;
    audio_user_ = user;
    return ME_OK;
}

me_status_t Player::set_external_clock_callback(me_player_external_clock_cb cb,
                                                  void* user) {
    /* Storage lives on PlaybackClock — `current()` reads the pair
     * under its own mutex on every pacer tick, so we don't snapshot
     * here. cb=NULL clears (resets to WALL fallback). */
    clock_.set_external_clock(cb, user);
    /* Wake any waiting threads in case the registration toggles
     * EXTERNAL's "no callback yet" → "callback now". The pacer's
     * wait_for window is short enough that this is belt-and-suspenders,
     * but cheap. */
    state_cv_.notify_all();
    return ME_OK;
}

void Player::notify_state_changed_locked() {
    state_cv_.notify_all();
}

}  // namespace me::orchestrator
