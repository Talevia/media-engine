/* C API for me_player — see include/media_engine/player.h.
 *
 * The opaque me_player handle wraps the C++ orchestrator::Player.
 * Borrowing the timeline pointer mirrors me_render_frame
 * (src/api/render.cpp) — me_player_create snapshots a shared_ptr
 * with a no-op deleter so lifetime stays with the caller's
 * me_timeline_t handle. */

#include "media_engine/player.h"
#include "core/engine_impl.hpp"
#include "orchestrator/player.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>
#include <utility>

struct me_player {
    std::unique_ptr<me::orchestrator::Player> impl;
};

namespace {

std::shared_ptr<const me::Timeline> borrow_timeline(const me_timeline_t* h) {
    return std::shared_ptr<const me::Timeline>(&h->tl, [](const me::Timeline*) {});
}

}  // namespace

extern "C" me_status_t me_player_create(
    me_engine_t*               engine,
    const me_timeline_t*       timeline,
    const me_player_config_t*  config,
    me_player_t**              out) {

    if (!engine || !timeline || !out) return ME_E_INVALID_ARG;
    *out = nullptr;
    me::detail::clear_error(engine);

    me_player_config_t cfg{};
    if (config) cfg = *config;

    if (cfg.master_clock == ME_CLOCK_EXTERNAL) {
        me::detail::set_error(engine,
            "me_player_create: ME_CLOCK_EXTERNAL not implemented this milestone");
        return ME_E_UNSUPPORTED;
    }

    auto wrapper = std::make_unique<me_player>();
    wrapper->impl = std::make_unique<me::orchestrator::Player>(
                        engine, borrow_timeline(timeline), cfg);
    *out = wrapper.release();
    return ME_OK;
}

extern "C" void me_player_destroy(me_player_t* p) {
    delete p;
}

extern "C" me_status_t me_player_set_video_callback(
    me_player_t*        p,
    me_player_video_cb  cb,
    void*               user) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->set_video_callback(cb, user);
}

extern "C" me_status_t me_player_set_audio_callback(
    me_player_t*        p,
    me_player_audio_cb  cb,
    void*               user) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->set_audio_callback(cb, user);
}

extern "C" me_status_t me_player_play(me_player_t* p, float rate) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->play(rate);
}

extern "C" me_status_t me_player_pause(me_player_t* p) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->pause();
}

extern "C" me_status_t me_player_seek(me_player_t* p, me_rational_t time) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->seek(time);
}

extern "C" me_status_t me_player_report_audio_playhead(
    me_player_t*  p,
    me_rational_t t) {
    if (!p || !p->impl) return ME_E_INVALID_ARG;
    return p->impl->report_audio_playhead(t);
}

extern "C" me_rational_t me_player_current_time(const me_player_t* p) {
    if (!p || !p->impl) return me_rational_t{0, 1};
    return p->impl->current_time();
}

extern "C" int me_player_is_playing(const me_player_t* p) {
    if (!p || !p->impl) return 0;
    return p->impl->is_playing() ? 1 : 0;
}
