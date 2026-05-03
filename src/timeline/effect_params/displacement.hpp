/* `EffectKind::Displacement` typed parameters — M12 §158
 * (2/2 geometric effects).
 *
 * Texture-driven displacement map: a still image (PNG / WebP /
 * JPEG, decoded via `compose/sticker_decoder.{hpp,cpp}`) drives
 * per-pixel offsets. Convention:
 *
 *   tex_R, tex_G ∈ [0, 255], where 128 = no displacement
 *   offset_x_px = ((tex_R * 2 - 255) * strength_x) / 255
 *   offset_y_px = ((tex_G * 2 - 255) * strength_y) / 255
 *   source = (x + offset_x_px, y + offset_y_px)
 *
 * The texture is mapped to the output dimensions: tex coord
 * (0, 0) = output (0, 0); tex coord (Tw-1, Th-1) = output
 * (W-1, H-1). Bilinear sampling for both texture lookup and
 * source sampling. Edges clamp.
 *
 * `texture_uri` empty OR (strength_x == 0 AND strength_y == 0)
 * → identity early-out. */
#pragma once

#include <string>

namespace me {

struct DisplacementEffectParams {
    /* file:// URI to the displacement texture. Empty = identity. */
    std::string texture_uri;
    /* Maximum offset (pixels) when texture R/G channels are at
     * extremes (0 → -strength, 255 → +strength). */
    float       strength_x = 0.0f;
    float       strength_y = 0.0f;
};

}  // namespace me
