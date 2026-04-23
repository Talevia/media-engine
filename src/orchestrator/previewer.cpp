#include "orchestrator/previewer.hpp"

namespace me::orchestrator {

me_status_t Previewer::frame_at(me_rational_t, me_frame** out_frame) {
    /* STUB: frame-server-impl — single-frame graph eval path; M6 milestone. */
    if (out_frame) *out_frame = nullptr;
    return ME_E_UNSUPPORTED;
}

}  // namespace me::orchestrator
