/*
 * me::effect::NoopGpuEffect — no-op GpuEffect.
 *
 * Analogue of `IdentityEffect` for the GPU path. Exists to prove
 * end-to-end GpuEffectChain plumbing (append → submit → the right
 * view / framebuffer state lands) before any real effect ships.
 * `submit` does not issue a draw call; it just binds the dst
 * framebuffer + clears + touches the view so bgfx's command stream
 * sees evidence the pass was submitted.
 *
 * Not useful for rendering — the clear color wipes the input rather
 * than passing it through. Once a real GpuEffect (color-correct)
 * lands, this stays around as a test fixture for chain-orchestrator
 * behaviour (e.g. "noop in the middle of a chain is a valid pass;
 * ping-pong arithmetic still matches up").
 */
#pragma once

#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

namespace me::effect {

class NoopGpuEffect final : public GpuEffect {
public:
    void submit(bgfx::ViewId            view_id,
                bgfx::TextureHandle     /*src*/,
                bgfx::FrameBufferHandle dst) const override {
        /* No real draw — just a view touch so the chain's view-id
         * allocation + framebuffer binding is exercised. Clears to
         * black so a test inspecting the output distinguishes
         * "noop ran" from "uninitialized garbage". */
        bgfx::setViewFrameBuffer(view_id, dst);
        bgfx::setViewClear(view_id, BGFX_CLEAR_COLOR, /*rgba=*/0x000000ff,
                           /*depth=*/1.0f, /*stencil=*/0);
        bgfx::touch(view_id);
    }

    const char* kind() const noexcept override { return "noop-gpu"; }
};

}  // namespace me::effect
