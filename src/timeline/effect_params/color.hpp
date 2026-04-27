/* `EffectKind::Color` typed parameters — extracted from the
 * `timeline_ir_params.hpp` umbrella header (debt-split-...). The
 * variant index 0 in `EffectSpec::params` resolves to this struct,
 * so its position relative to the EffectKind enum value is the
 * single binding invariant. */
#pragma once

namespace me {

/* Brightness / contrast / saturation triplet — per-pixel byte
 * arithmetic, no spatial neighbourhood read. Ranges documented
 * are *semantic* and not loader-enforced — downstream GPU
 * effects clamp to their shader's valid domain. */
struct ColorEffectParams {
    double brightness = 0.0;   /* ~[-1, +1]; 0 = identity */
    double contrast   = 1.0;   /* ~[0, 2];   1 = identity */
    double saturation = 1.0;   /* ~[0, 2];   1 = identity */
};

}  // namespace me
