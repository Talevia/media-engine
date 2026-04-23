#include "media_engine/timeline.h"
#include "core/engine_impl.hpp"
#include "core/engine_seed.hpp"
#include "timeline/timeline_impl.hpp"
#include "timeline/timeline_loader.hpp"

#include <string>
#include <string_view>

extern "C" me_status_t me_timeline_load_json(
    me_engine_t* engine, const char* json, size_t len, me_timeline_t** out) {
    if (!engine || !json || !out) return ME_E_INVALID_ARG;
    me::detail::clear_error(engine);

    std::string err;
    me_status_t s = me::timeline::load_json(std::string_view(json, len), out, &err);
    if (s != ME_OK) {
        me::detail::set_error(engine, std::move(err));
        return s;
    }
    /* Every per-engine side effect of a successful Timeline load lives
     * behind this single call (asset-hash seed today; M2 color pipeline
     * / M3 effect-LUT preheat tomorrow). Keeping the extern C entry a
     * glue one-liner is the point — see `src/core/engine_seed.hpp`. */
    if (*out) {
        me::detail::seed_engine_from_timeline(*engine, (*out)->tl);
    }
    return ME_OK;
}

extern "C" void me_timeline_destroy(me_timeline_t* tl) {
    delete tl;
}

extern "C" me_rational_t me_timeline_duration(const me_timeline_t* tl) {
    return tl ? tl->tl.duration : me_rational_t{0, 1};
}

extern "C" me_rational_t me_timeline_frame_rate(const me_timeline_t* tl) {
    return tl ? tl->tl.frame_rate : me_rational_t{30, 1};
}

extern "C" void me_timeline_resolution(const me_timeline_t* tl, int* width, int* height) {
    if (width)  *width  = tl ? tl->tl.width  : 0;
    if (height) *height = tl ? tl->tl.height : 0;
}
