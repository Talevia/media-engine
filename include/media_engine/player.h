/*
 * Player — timeline preview playback session.
 *
 * Two-roles-two-paths companion to me_render_frame:
 *
 *   Frame    : me_render_frame  → synchronous single-frame pull at any t.
 *              Stateless. Use for thumbnails / scrubbing / agent-driven
 *              previews where the host owns the clock.
 *
 *   Player   : me_player_*      → stateful playback session with internal
 *              clock + A/V sync. Use when the host wants timeline-style
 *              play/pause/seek and wants the engine to decide when each
 *              video frame should be presented and which audio chunks
 *              should play next. Host owns the surface (window / widget),
 *              engine owns the session.
 *
 * Both modes share the per-frame video graph (compile_frame_graph) and
 * the same scheduler / cache layers. See docs/ARCHITECTURE_GRAPH.md
 * §三种执行模型 (c) for the architectural rationale; M8 in
 * docs/MILESTONES.md for exit criteria.
 *
 * Threading
 * ---------
 *   - me_player_create / destroy / play / pause / seek may be called
 *     from any thread.
 *   - Video / audio callbacks are invoked on engine-owned threads; do
 *     NOT do synchronous GPU submission or other long-blocking work
 *     inside the callback. Copy out what you need and return promptly.
 *   - me_player_report_audio_playhead is intended to be called from the
 *     host's audio device callback (real-time audio thread) — it is
 *     lock-free in the steady state.
 */
#ifndef MEDIA_ENGINE_PLAYER_H
#define MEDIA_ENGINE_PLAYER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct me_player me_player_t;

/* --- Master clock -------------------------------------------------------- */

typedef enum me_master_clock_kind {
    /* Auto-pick: AUDIO if the timeline has any audio track, else WALL.
     * Selected at me_player_create time. */
    ME_CLOCK_AUTO     = 0,

    /* Audio playhead is the master. Host MUST call
     * me_player_report_audio_playhead from its audio device callback
     * for video pacing to track audio. Until the first report lands
     * the player falls back to WALL. */
    ME_CLOCK_AUDIO    = 1,

    /* std::chrono::steady_clock is the master. Use when the host has
     * no audio device or only video matters. */
    ME_CLOCK_WALL     = 2,

    /* Host-driven external clock (e.g. SMPTE LTC, MIDI clock,
     * cross-process IPC). The host calls
     * me_player_set_external_clock_callback before me_player_play;
     * the engine calls that callback whenever the pacer needs the
     * current time. Until a callback is set, current() falls back
     * to WALL projection, so an unwired ME_CLOCK_EXTERNAL behaves
     * like ME_CLOCK_WALL with a freshly-seeded anchor. */
    ME_CLOCK_EXTERNAL = 3
} me_master_clock_kind_t;

/* --- Audio output spec --------------------------------------------------- */

/* What the host's audio device wants. The engine produces planar
 * float (FLTP) samples at this rate / channel count. Set
 * sample_rate=0 to disable audio entirely (forces ME_CLOCK_WALL). */
typedef struct me_audio_output_spec {
    int32_t sample_rate;     /* e.g. 48000; 0 = no audio */
    int32_t num_channels;    /* e.g. 2 */
} me_audio_output_spec_t;

/* --- Player config ------------------------------------------------------- */

typedef struct me_player_config {
    me_audio_output_spec_t  audio_out;
    me_master_clock_kind_t  master_clock;

    /* Video frame ring capacity (frames). 0 → default 3. */
    int32_t                 video_ring_capacity;

    /* Audio queue depth in milliseconds. 0 → default 200. */
    int32_t                 audio_queue_ms;
} me_player_config_t;

/* --- Frame / audio callbacks --------------------------------------------- */

/* Invoked by the pacer thread when `frame` should be visible to the
 * user at timeline time `present_at`. The frame pointer is borrowed —
 * valid only for the duration of the callback; if the host needs it
 * past return, copy out the pixels. */
typedef void (*me_player_video_cb)(
    const me_frame_t*  frame,
    me_rational_t      present_at,
    void*              user);

/* Invoked when a fresh chunk of mixed audio is ready. `planes` points
 * at `num_channels` parallel float arrays of `num_samples` samples
 * each (FLTP layout). Buffer is borrowed for the call. `chunk_start`
 * is the timeline time of the first sample. */
typedef void (*me_player_audio_cb)(
    const float* const* planes,
    int                 num_channels,
    int                 num_samples,
    me_rational_t       chunk_start,
    void*               user);

/* --- Lifecycle ----------------------------------------------------------- */

/* Create a player bound to `timeline`. The timeline must outlive the
 * player. `config` is copied; pass NULL for defaults (no audio output,
 * AUTO master clock, ring=3, queue=200ms). On error returns non-OK
 * and *out is left null; me_engine_last_error has the message. */
ME_API me_status_t me_player_create(
    me_engine_t*               engine,
    const me_timeline_t*       timeline,
    const me_player_config_t*  config,
    me_player_t**              out);

/* Tear down. Joins all internal threads; safe to call from any thread
 * other than the player's own callback. NULL-safe. */
ME_API void me_player_destroy(me_player_t* p);

/* --- Callback registration ----------------------------------------------- */

/* Set or clear (`cb=NULL`) the video callback. May be called any time;
 * the change becomes visible to the next frame the pacer dispatches. */
ME_API me_status_t me_player_set_video_callback(
    me_player_t*        p,
    me_player_video_cb  cb,
    void*               user);

ME_API me_status_t me_player_set_audio_callback(
    me_player_t*        p,
    me_player_audio_cb  cb,
    void*               user);

/* Required by ME_CLOCK_EXTERNAL. The pacer thread calls `cb(user)`
 * whenever it needs the current playhead in timeline coordinates
 * (typically every ~10 ms while playing). The callback MUST be
 * lock-free + fast — blocking it stalls video pacing. Return
 * monotonic non-decreasing values across calls; a regression looks
 * like a backward seek to the engine and trips the pacer's
 * "drop-stale" branch. Setting cb=NULL clears the callback;
 * subsequent queries fall back to WALL projection until a fresh
 * callback is registered. May be called any time; the change is
 * picked up on the next pacer tick. No-op for non-EXTERNAL master
 * clocks (registered but never queried). */
typedef me_rational_t (*me_player_external_clock_cb)(void* user);

ME_API me_status_t me_player_set_external_clock_callback(
    me_player_t*                  p,
    me_player_external_clock_cb   cb,
    void*                         user);

/* --- Transport ----------------------------------------------------------- */

/* Begin or resume playback at `rate`.
 *
 *   rate ==  1.0f      → normal playback.
 *   rate ∈ [0.5, 2.0]  → variable-rate forward playback. The master
 *                        clock projects elapsed wall-clock time × rate;
 *                        the pacer drops/holds frames against the
 *                        scaled clock — slow-motion holds each frame
 *                        for longer; fast-forward drops behind frames.
 *                        Only allowed when the timeline has no audio
 *                        track and the master clock is not ME_CLOCK_AUDIO;
 *                        audio + variable rate without time-stretching
 *                        would desync against the host audio device.
 *   rate <= 0          → ME_E_INVALID_ARG (zero is what pause() is for;
 *                        negative is reverse playback, a separate
 *                        follow-up needing reverse demux).
 *   rate outside [0.5, 2.0] OR
 *   audio + rate ≠ 1   → ME_E_UNSUPPORTED. */
ME_API me_status_t me_player_play(me_player_t* p, float rate);

/* Pause playback. Producer thread parks; the current frame stays
 * registered; resume continues from current_time. */
ME_API me_status_t me_player_pause(me_player_t* p);

/* Move the playhead to `time`. Drops all in-flight + queued frames /
 * audio. After return, the next callback fires for a frame at (or
 * very near) `time`. Safe to call while playing or paused. */
ME_API me_status_t me_player_seek(me_player_t* p, me_rational_t time);

/* Required by ME_CLOCK_AUDIO. Host calls this from its audio device
 * callback with the timeline-coordinate position of the sample
 * currently emerging from the speaker. The video pacer chases this
 * value. No-op for ME_CLOCK_WALL. */
ME_API me_status_t me_player_report_audio_playhead(
    me_player_t*    p,
    me_rational_t   time);

/* --- Introspection ------------------------------------------------------- */

ME_API me_rational_t me_player_current_time(const me_player_t* p);
ME_API int           me_player_is_playing(const me_player_t* p);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_PLAYER_H */
