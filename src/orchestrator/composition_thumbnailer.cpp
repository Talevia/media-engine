#include "orchestrator/composition_thumbnailer.hpp"

namespace me::orchestrator {

me_status_t CompositionThumbnailer::png_at(me_rational_t, int, int,
                                            uint8_t** out_png, size_t* out_size) {
    /* STUB: composition-thumbnail-impl — timeline-driven (composition-level)
     * thumbnail path; awaits the M6 frame server as its only consumer. The
     * asset-level C API (me_thumbnail_png) is a separate, fully-implemented
     * path that does NOT go through this class. */
    if (out_png)  *out_png  = nullptr;
    if (out_size) *out_size = 0;
    return ME_E_UNSUPPORTED;
}

}  // namespace me::orchestrator
