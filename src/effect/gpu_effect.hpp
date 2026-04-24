/*
 * me::effect::GpuEffect — abstract base for GPU-side compositing effects.
 *
 * Parallel structure to `me::effect::Effect` (CPU-side, `effect.hpp`):
 * both exist so compose-sink can pick the right path at runtime
 * without cross-contaminating the types. A GPU effect's signal is
 * utterly different from a CPU one — framebuffer-in / framebuffer-out
 * rather than uint8_t* in-place — so they can't share a base class
 * without dragging virtual overhead + bgfx types into the CPU path
 * or stubbing everything with `unsupported on this path` branches.
 *
 * Compiled only under `-DME_WITH_GPU=ON` (the two GpuEffect headers
 * are added to `media_engine`'s source list inside
 * `$<$<BOOL:${ME_WITH_GPU}>:...>` generator expressions — see
 * `src/CMakeLists.txt`). No ME_HAS_GPU guards inside the header
 * itself: if a TU is including this it already knows it's on the
 * GPU path.
 *
 * Contract:
 *   - `submit(view_id, src, dst)`: queue one pass on the given bgfx
 *     view. `src` is the input sampler (bound to stage 0 by
 *     convention); `dst` is the target framebuffer for the pass.
 *     The effect is responsible for setting view framebuffer +
 *     viewport rect + its own shader program + uniforms + state +
 *     vertex buffers + issuing a draw / submit. Callers (e.g.
 *     GpuEffectChain) do not touch view state themselves, so effects
 *     can use `bgfx::setViewClear` / `bgfx::setViewRect` / etc.
 *     without worrying about collision with sibling effects.
 *   - `kind()`: short debug-visible tag, like the CPU Effect's tag.
 *
 * Not abstract on purpose (in this skeleton):
 *   - Program and uniform accessors (the draft bullet mentioned
 *     `program()` + `set_uniforms()`). Deliberately pushed inside
 *     each derived class instead of hoisting to the base. Rationale:
 *     the exact shader architecture — single program vs
 *     program-per-variant, uniform-per-effect vs block-shared —
 *     won't stabilize until a couple of real effects exist. Once
 *     EffectChain pass-merge needs program introspection to fuse
 *     shaders, we add the virtual back.
 *
 * Ownership: GpuEffect instances are owned by their GpuEffectChain
 * via unique_ptr (same pattern as CPU EffectChain). Each effect owns
 * its own bgfx resource handles (programs, uniforms) and releases
 * them in its dtor. Safe to construct / destroy without an active
 * bgfx frame — bgfx::createProgram / bgfx::destroy are fine any
 * time after bgfx::init.
 *
 * Thread-safety: must be called on the bgfx API thread. Once
 * `bgfx-render-thread-pin` lands, that thread is a dedicated one
 * owned by the GpuBackend; until then callers are responsible.
 */
#pragma once

#include <bgfx/bgfx.h>

namespace me::effect {

class GpuEffect {
public:
    virtual ~GpuEffect() = default;

    virtual void submit(bgfx::ViewId            view_id,
                        bgfx::TextureHandle     src,
                        bgfx::FrameBufferHandle dst) const = 0;

    virtual const char* kind() const noexcept = 0;
};

}  // namespace me::effect
