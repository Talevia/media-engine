/* `me::compose::Bbox` — pre-resolved 2D bounding box in image
 * pixel space. Lightweight POD shared by the M11 face-/body-
 * effect kernels (`face_sticker`, `face_mosaic`, `body_alpha_key`)
 * as a uniform "where to apply" input.
 *
 * Why a shared type. All three M11 kernels were originally
 * written to pull landmark/mask data out of an asset_id string
 * via an inference resolver that doesn't exist yet. The kernels
 * have been refactored to accept pre-resolved bboxes instead;
 * upstream resolution (running BlazeFace + projecting landmarks
 * → bbox, or reading a static fixture asset) is a separate
 * compose-graph stage tracked under
 * `face-mosaic-resolver-wiring` / `body-mask-resolver-wiring`.
 *
 * Coordinate system: image-space pixel coordinates with origin
 * top-left, x right, y down. `x0` / `y0` is the top-left corner
 * (inclusive); `x1` / `y1` is the bottom-right corner (exclusive
 * — i.e. half-open intervals so width = x1 - x0). Negative or
 * zero-area bboxes are valid no-ops; the kernels skip them
 * rather than rejecting (resolver layers may emit empty boxes
 * when landmark confidence is below threshold).
 *
 * Float-vs-int: int32_t to keep the bbox a trivial copy. Sub-
 * pixel precision isn't needed at the bbox level — the kernels
 * already do nearest-neighbor sampling within the box (M11
 * effect quality bar; bilinear is a future deepening).
 */
#pragma once

#include <cstdint>

namespace me::compose {

struct Bbox {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;

    constexpr std::int32_t width()  const noexcept { return x1 > x0 ? x1 - x0 : 0; }
    constexpr std::int32_t height() const noexcept { return y1 > y0 ? y1 - y0 : 0; }
    constexpr bool         empty()  const noexcept { return width() == 0 || height() == 0; }
};

}  // namespace me::compose
