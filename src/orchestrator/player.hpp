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

#include "audio/mixer.hpp"
#include "io/demux_context.hpp"
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

    /* Resolve the active video clip + asset URI + clip-local source
     * time at timeline-coordinate `t`. Mirrors Previewer::frame_at's
     * single-bottom-track lookup; multi-track compose is the same
     * follow-up that lifts Previewer (see BACKLOG
     * `previewer-multi-track-compose-graph`). Returns ME_OK with
     * outputs populated, ME_E_NOT_FOUND if no clip covers `t`. */
    me_status_t resolve_active_clip(me_rational_t       t,
                                     std::string*       out_uri,
                                     me_rational_t*     out_source_t) const;

    /* Render one video frame at clip-local time `source_t` for `uri`.
     * Wraps compile_frame_graph + scheduler.evaluate_port. */
    me_status_t render_one(const std::string&                            uri,
                            me_rational_t                                  source_t,
                            std::shared_ptr<me::graph::RgbaFrameData>*    out,
                            std::string*                                  err) const;

    /* Open one DemuxContext per audio clip via the IoDemux kernel
     * (mirrors Exporter's per-clip demux fan-out at exporter.cpp:215).
     * Slot is null for non-audio clips. Lifetime spans the player —
     * AudioMixer + AudioTrackFeed hold shared_ptr into this vector. */
    me_status_t open_audio_demuxes(std::string* err);

    /* Build (or rebuild) the AudioMixer from `audio_demuxes_` using the
     * configured audio_out spec as the mixer's target format. Caller
     * is responsible for stalling audio_producer_ across the rebuild
     * (Phase 5 will use this on seek). */
    me_status_t setup_audio_mixer(std::string* err);

    me_engine*                              engine_     = nullptr;
    std::shared_ptr<const me::Timeline>     tl_;
    me_player_config_t                      cfg_{};

    /* Frame period for the producer cursor advance. Captured from
     * tl_->frame_rate at construct time. */
    me_rational_t                           frame_period_{1, 30};

    PlaybackClock                           clock_;
    VideoFrameRing                          ring_;

    /* Audio path. Built lazily in ctor when:
     *   - audio_out.sample_rate > 0, AND
     *   - the timeline has at least one Audio track with clips.
     * `audio_mixer_` null otherwise → audio_producer_loop early-exits.
     * `audio_demuxes_` is parallel to tl_->clips (size match); audio
     * clips have non-null entries, others null. */
    std::vector<std::shared_ptr<me::io::DemuxContext>> audio_demuxes_;
    std::unique_ptr<me::audio::AudioMixer>             audio_mixer_;
    /* Cumulative samples this Player has dispatched to the host's
     * audio cb. chunk_start = audio_dispatched_samples_ / sample_rate.
     * Independent of AudioMixer's internal samples_emitted_ counter
     * so seek (Phase 5) can reset the host-facing timestamp without
     * touching the mixer. */
    int64_t                                            audio_dispatched_samples_ = 0;

    /* State protected by mu_. */
    mutable std::mutex                      mu_;
    std::condition_variable                 state_cv_;
    me_rational_t                           produce_cursor_{0, 1};
    bool                                    shutdown_     = false;

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
