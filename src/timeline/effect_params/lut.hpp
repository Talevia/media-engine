/* `EffectKind::Lut` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 2. */
#pragma once

#include <string>

namespace me {

struct LutEffectParams {
    std::string path;          /* .cube file path / URI; asset_ref
                                * resolution deferred to LUT effect */
};

}  // namespace me
