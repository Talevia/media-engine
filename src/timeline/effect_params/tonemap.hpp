/* `EffectKind::Tonemap` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 3.
 *
 * HDR → SDR tonemap effect parameters. Three industry-standard
 * curves; the host picks one + a target white point. M10 exit
 * criterion 6 (`tonemap-effect-hable`).
 *
 * Algorithms (all pure functions of the per-channel sample value;
 * no spatial neighbourhood read so they're trivially deterministic
 * and independent of stride/dimensions):
 *
 *   Hable    — Hable filmic ("Uncharted 2") curve. Strong
 *              highlight roll-off + gentle shadow lift; the
 *              colour-grader-favourite default.
 *   Reinhard — `x / (1 + x)`. Simplest, very gentle roll-off,
 *              good when the input is mildly over-bright but
 *              not aggressively HDR.
 *   ACES     — ACES filmic approximation (Knarkowicz fit).
 *              Closer to industry-standard ACES output but
 *              slightly clippy on saturated reds.
 *
 * `target_nits` (≈ 100 cd/m² for SDR Rec.709 displays) sets the
 * mapping reference: input values are interpreted as linear
 * luminance in [0, target_nits / 100] before the curve, then
 * scaled back to RGBA8 [0, 255]. Values > 0 only.
 *
 * Engine note. The compose path's working buffer is RGBA8 (per
 * `me::compose::frame_to_rgba8`), so this effect operates on
 * already-quantised SDR samples — it's a creative-look operation
 * within the SDR domain, with `target_nits` controlling how
 * aggressively highlights roll off. True HDR-precision (linear-
 * light float / RGBA16) tonemapping waits on the working-buffer
 * upgrade in M11+. The output is byte-identical for the same
 * input + algo + target_nits, so the deterministic-software-path
 * contract (VISION §3.1 / §5.3) holds today. */
#pragma once

#include <cstdint>

namespace me {

struct TonemapEffectParams {
    enum class Algo : uint8_t {
        Hable    = 0,
        Reinhard = 1,
        ACES     = 2,
    };
    Algo   algo        = Algo::Hable;
    double target_nits = 100.0;   /* SDR Rec.709 display reference */
};

}  // namespace me
