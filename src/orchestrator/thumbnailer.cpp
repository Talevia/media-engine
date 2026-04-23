#include "orchestrator/thumbnailer.hpp"

namespace me::orchestrator {

me_status_t Thumbnailer::png_at(me_rational_t, int, int,
                                 uint8_t** out_png, size_t* out_size) {
    /* STUB: composition-thumbnail-impl — the timeline-driven thumbnail
     * path. The asset-level C API (me_thumbnail_png) already works and
     * bypasses this class; see PAIN_POINTS for the signature mismatch. */
    if (out_png)  *out_png  = nullptr;
    if (out_size) *out_size = 0;
    return ME_E_UNSUPPORTED;
}

}  // namespace me::orchestrator
