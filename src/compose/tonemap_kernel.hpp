/*
 * tonemap_kernel — CPU tonemap pass over an RGBA8 buffer in-place.
 *
 * M10 exit criterion 6 first half — the SDR↔HDR effect kind that
 * `parse_effect_spec` accepts as `kind: "tonemap"`. Three industry-
 * standard curves selected via `TonemapEffectParams::Algo` (Hable
 * filmic / Reinhard / ACES); `target_nits` controls how aggressively
 * highlights roll off relative to the SDR Rec.709 reference white
 * (100 cd/m²).
 *
 * Engine note (see TonemapEffectParams docs in timeline_ir_params.hpp).
 * The compose path's working buffer is RGBA8, so this kernel
 * operates on already-quantised SDR samples — interpreting the [0,
 * 255] byte as linear-light luminance scaled into
 * [0, target_nits / 100], applying the selected curve, scaling back
 * to byte. True HDR-precision (linear-light float / RGBA16)
 * tonemapping waits on the working-buffer upgrade in M11+. The
 * output is byte-identical for the same input + algo + target_nits,
 * so the deterministic-software-path contract (VISION §3.1 / §5.3)
 * holds today.
 *
 * Determinism. All three curves are pure per-channel float math; no
 * spatial neighbourhood read, no parallelism, no floating-point FMA
 * dispatch (the compiled shape is plain `*` and `+`). Same input →
 * same bytes across runs and architectures.
 *
 * Alpha. Pass-through. Tonemapping is a luminance / chrominance
 * operation; the alpha channel is not photometric data and is
 * preserved verbatim.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Apply tonemap in-place. `rgba` is row-major RGBA8 with stride
 * `stride_bytes` (= width * 4 for tightly-packed buffers; arbitrary
 * larger stride accepted for future flexibility). Returns ME_OK on
 * normal apply; ME_E_INVALID_ARG when `rgba` is null, dimensions
 * are non-positive, stride is < width * 4, or `target_nits <= 0`. */
me_status_t apply_tonemap_inplace(std::uint8_t*                       rgba,
                                   int                                 width,
                                   int                                 height,
                                   std::size_t                         stride_bytes,
                                   const me::TonemapEffectParams&      params);

}  // namespace me::compose
