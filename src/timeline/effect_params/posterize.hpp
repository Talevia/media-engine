/* `EffectKind::Posterize` typed parameters — M12 §156
 * (4/5 stylized effects).
 *
 * Per-channel quantization to N evenly-spaced levels. The
 * standard "posterize" reduction: input byte → bucket index
 * → bucket-representative byte. levels=2 gives binary R/G/B
 * (each channel either 0 or 255 depending on whether the
 * input was below/above 128). levels=256 = identity.
 *
 * Default-constructed param is identity (levels=256); the
 * kernel recognizes this and skips the per-pixel transform. */
#pragma once

namespace me {

struct PosterizeEffectParams {
    /* Number of quantization levels per channel. Range
     * [2, 256]. levels=256 = identity. levels=2 = binary
     * (pure black or 255-saturated per channel). */
    int levels = 256;
};

}  // namespace me
