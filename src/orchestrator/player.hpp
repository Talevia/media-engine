/*
 * Player — timeline preview playback session orchestrator.
 *
 * Holds (current_time, rate, is_playing) state plus two internal
 * threads:
 *
 *   Producer : finds the active clip at `produce_cursor`, compiles
 *              compile_frame_graph for it, evaluates via the engine's
 *              scheduler, pushes (timeline_t, RgbaFrameData) into the
 *              VideoFrameRing. Audio production is wired in a follow-
 *              up phase (Phase 3 of the player refactor plan).
 *   Pacer    : pops the next slot from the ring, waits until the
 *              master clock reaches that slot's present_at, then
 *              invokes the host's video callback. Drops slots that
 *              are far behind the clock (chase-audio basis, used
 *              meaningfully once AUDIO master clock arrives in Phase 4).
 *
 * Audio (Phase 3) and chase-audio sync (Phase 4) plug into this
 * skeleton without changing its threading model. Cooperative cancel
 * on seek (Phase 5) replaces the timestamp-based drop with explicit
 * Future::cancel calls.
 *
 * See docs/ARCHITECTURE_GRAPH.md §三种执行模型 (c) for the model and
 * docs/MILESTONES.md M8 for the exit criteria.
 */
#pragma once

#include "graph/future.hpp"
#include "graph/types.hpp"
#include "media_engine/player.h"
#include "media_engine/types.h"
#include "orchestrator/playback_clock.hpp"
#include "orchestrator/video_frame_ring.hpp"
#include "timeline/timeline_impl.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct me_engine;

namespace me::orchestrator {

class Player {
public:
    Player(me_engine*                            engine,
           std::shared_ptr<const me::Timeline>   timeline,
           const me_player_config_t&             cfg);

    /* Destructor stops both threads and joins. */
    ~Player();

    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;

    /* C API surface — implementations match me_player_*. */
    me_status_t   set_video_callback(me_player_video_cb cb, void* user);
    me_status_t   set_audio_callback(me_player_audio_cb cb, void* user);
    me_status_t   play(float rate);
    me_status_t   pause();
    me_status_t   seek(me_rational_t time);
    me_status_t   report_audio_playhead(me_rational_t t);
    me_rational_t current_time() const;
    bool          is_playing()  const;

private:
    /* Thread bodies. */
    void producer_loop();
    void pacer_loop();
    void audio_producer_loop();

    /* Ask the producer thread to wake (state changed: play / pause /
     * seek / shutdown). */
    void notify_state_changed_locked();

    me_engine*                              engine_     = nullptr;
    std::shared_ptr<const me::Timeline>     tl_;
    me_player_config_t                      cfg_{};

    /* Frame period for the producer cursor advance. Captured from
     * tl_->frame_rate at construct time. */
    me_rational_t                           frame_period_{1, 30};

    PlaybackClock                           clock_;
    VideoFrameRing                          ring_;

    /* Audio path — kernel-graph-based as of player-audio-kernel-ize
     * commit. The audio_producer_loop calls
     * compile_audio_chunk_graph(*tl_, audio_chunk_cursor_, …) per
     * chunk and evaluates through engine_->scheduler. State held
     * here:
     *   - has_audio_track_: gate the audio thread spawn / loop
     *     (set at ctor by scanning tl_->tracks).
     *   - audio_chunk_cursor_: timeline-global time of the next
     *     chunk to compile. Advanced by chunk_nb_samples /
     *     sample_rate per emitted chunk; reset on seek().
     *   - audio_dispatched_samples_: cumulative samples emitted to
     *     the host cb. Used both to compute the host-visible chunk
     *     start timestamp AND for the queue-ahead-of-clock
     *     throttle. Reset on seek() to time × sr. */
    bool                                    has_audio_track_ = false;
    me_rational_t                           audio_chunk_cursor_{0, 1};
    int64_t                                 audio_dispatched_samples_ = 0;

    /* State protected by mu_. */
    mutable std::mutex                      mu_;
    std::condition_variable                 state_cv_;
    me_rational_t                           produce_cursor_{0, 1};
    bool                                    shutdown_     = false;

    /* Bumped on every seek. Producer captures the value at the start
     * of an iteration; if seek_epoch_ has changed by the time the
     * graph evaluation finishes, the produced frame is for a stale
     * cursor and gets dropped before push. Pairs with the
     * cooperative Future::cancel below — together they cover both
     * "seek arrived after submit" (cancel wakes await) and
     * "seek arrived after compile but before submit" (epoch check). */
    int64_t                                 seek_epoch_   = 0;

    /* Held while the producer's video graph evaluation is in flight.
     * seek() calls cancel() on this so any blocking await unblocks
     * with ME_E_CANCELLED instead of finishing decode for a frame
     * we'll throw away. Reset by the producer as soon as await
     * returns (success or otherwise). */
    using VideoFuture = me::graph::Future<std::shared_ptr<me::graph::RgbaFrameData>>;
    std::optional<VideoFuture>              in_flight_video_;

    /* Callbacks may be replaced at runtime. video_cb_ / audio_cb_ are
     * read by the pacer; mu_ guards reassignment. */
    me_player_video_cb                      video_cb_     = nullptr;
    void*                                   video_user_   = nullptr;
    me_player_audio_cb                      audio_cb_     = nullptr;
    void*                                   audio_user_   = nullptr;

    /* Worker threads — started in ctor, joined in dtor. The
     * audio_producer_ thread is spawned only if audio_mixer_ was
     * built; otherwise it stays default-constructed (joinable() is
     * false → dtor's join is a no-op). */
    std::thread                             producer_;
    std::thread                             pacer_;
    std::thread                             audio_producer_;
};

}  // namespace me::orchestrator
