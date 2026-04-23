#include "orchestrator/thumbnailer.hpp"

namespace me::orchestrator {

me_status_t Thumbnailer::png_at(me_rational_t, int, int,
                                 uint8_t** out_png, size_t* out_size) {
    if (out_png)  *out_png  = nullptr;
    if (out_size) *out_size = 0;
    return ME_E_UNSUPPORTED;
}

}  // namespace me::orchestrator
