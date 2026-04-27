/* `EffectKind::Blur` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 1. */
#pragma once

namespace me {

struct BlurEffectParams {
    double radius = 0.0;       /* pixels; 0 = identity */
};

}  // namespace me
