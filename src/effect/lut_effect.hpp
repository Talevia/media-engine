/*
 * me::effect::LutEffect — 3D-LUT color-grading GPU effect.
 *
 * Third real GpuEffect — closes M3's "≥ 3 GPU effect (blur /
 * color-correct / LUT)" exit criterion. Samples a cube-shaped
 * 3D texture at each fragment's RGB to remap color. Trilinear
 * filtering on the LUT sampler smooths across LUT cells.
 *
 * LUT data format:
 *   - cube size N: number of entries per axis (N³ RGB triples).
 *   - float RGB data: N*N*N*3 floats, ordered z-fastest-then-y-
 *     then-x — matching Adobe's .cube file layout (the typical
 *     source), the `ImageImaging` convention, and Metal's
 *     `MTLTextureType3D` row-major ordering.
 *   - Values unclamped float — caller should pre-clamp to their
 *     target color gamut.
 *
 * Loader path (.cube parser) lives in src/effect/lut.{hpp,cpp}.
 * LutEffect itself just takes the parsed cube size + data and
 * uploads it to bgfx as a 3D texture.
 *
 * Metal-only today (shader bytecode limitation); non-Metal
 * renderers get `valid() == false`, matching other GpuEffects.
 *
 * Ownership: ctor allocates a 3D texture, vertex buffer, shader
 * program, and two sampler uniforms. Ctor + dtor MUST run on the
 * bgfx API thread (via BgfxGpuBackend::submit_on_render_thread).
 */
#pragma once

#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace me::effect {

class LutEffect final : public GpuEffect {
public:
    /* Construct from parsed cube data. `cube_size` is the entries-
     * per-axis count (commonly 17, 33, or 65). `rgb_float_data`
     * must contain cube_size³ × 3 floats, ordered
     * data[((z*N + y)*N + x)*3 + c] for cell (x, y, z), channel c.
     *
     * If cube_size < 2 or data.size() doesn't match, ctor early-
     * returns with valid() == false. */
    LutEffect(int cube_size, std::vector<float> rgb_float_data);
    ~LutEffect() override;

    LutEffect(const LutEffect&)            = delete;
    LutEffect& operator=(const LutEffect&) = delete;

    void submit(bgfx::ViewId            view_id,
                bgfx::TextureHandle     src,
                bgfx::FrameBufferHandle dst) const override;

    const char* kind() const noexcept override { return "lut"; }

    bool valid() const noexcept { return valid_; }

    int  cube_size() const noexcept { return cube_size_; }

private:
    int                      cube_size_ = 0;
    std::vector<float>       lut_data_;   /* kept alive for bgfx makeRef */
    bool                     valid_     = false;
    bgfx::VertexBufferHandle vbh_       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      program_   = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle      lut_tex_   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      s_src_     = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      s_lut_     = BGFX_INVALID_HANDLE;
};

}  // namespace me::effect
