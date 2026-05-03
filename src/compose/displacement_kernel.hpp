/*
 * displacement_kernel — texture-driven per-pixel offset.
 *
 * M12 §158 (2/2 geometric effects). Pure-pixel kernel that
 * takes a separately-decoded RGBA8 displacement texture (caller
 * provides bytes; the stage layer is responsible for decoding
 * via compose/sticker_decoder.{hpp,cpp}). For each output pixel:
 *
 *   tex_R, tex_G = bilinear sample of texture at (x_norm, y_norm)
 *   offset_x_px = ((tex_R * 2 - 255) * strength_x) / 255
 *   offset_y_px = ((tex_G * 2 - 255) * strength_y) / 255
 *   source_pos  = (x + offset_x_px, y + offset_y_px)
 *   output      = bilinear sample of input at source_pos (clamped)
 *
 * Identity case: tex_R == tex_G == 128 everywhere (or strength
 * == 0) → offsets are zero → output equals input.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0          → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4            → ME_E_INVALID_ARG
 *   - tex_rgba == nullptr / tex_w<=0 / tex_h<=0 (when not identity)
 *                                           → ME_E_INVALID_ARG
 *
 * texture-NULL with strength != 0 returns INVALID_ARG; the stage
 * layer decides whether an empty texture URI is treated as
 * identity (it is — see displacement_stage.cpp).
 */
#pragma once

#include "media_engine/types.h"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_displacement_inplace(
    std::uint8_t*       rgba,
    int                 width,
    int                 height,
    std::size_t         stride_bytes,
    const std::uint8_t* tex_rgba,
    int                 tex_width,
    int                 tex_height,
    float               strength_x,
    float               strength_y);

}  // namespace me::compose
