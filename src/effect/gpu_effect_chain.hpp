/*
 * me::effect::GpuEffectChain — ordered chain of GpuEffects with
 * ping-pong scratch framebuffer management.
 *
 * Parallel to CPU `EffectChain`. Each GpuEffect runs as one pass on
 * its own bgfx view (view IDs allocated contiguously from
 * `first_view_id`). The chain arranges per-pass input / output
 * framebuffers so each effect reads from the prior effect's output
 * and the last effect writes to the caller-supplied `dst`.
 *
 * Ping-pong scheme:
 *   - N = 0: no-op (assertion-checked in debug; early-return in
 *     release).
 *   - N = 1: effect.submit(first_view, src, dst). No scratch used.
 *   - N = 2: effect[0] → scratch_a, effect[1] reads scratch_a's
 *     color texture → dst.
 *   - N = 3: ...[0] → scratch_a, ...[1] reads scratch_a → scratch_b,
 *     ...[2] reads scratch_b → dst.
 *   - N ≥ 4: alternate scratch_a / scratch_b per intermediate pass
 *     (effect i writes to scratch_a if i % 2 == 0 else scratch_b,
 *     skipping the final i = N-1 which writes to dst).
 *
 * Scratch contract: caller owns `scratch_a` / `scratch_b` and
 * guarantees their color attachment texture format matches `dst`'s
 * and their dimensions match `dst`'s (so each effect can assume a
 * fixed resolution; no per-pass resize). For N ≤ 1, pass
 * `BGFX_INVALID_HANDLE` for both scratches — asserted in debug.
 *
 * Future: `compile()` method for pass-merging same-shape effects
 * (the M3 "EffectChain 能把连续 ≥ 2 个像素级 effect 合并成单 pass"
 * exit criterion). Not wired yet — the per-effect-per-pass path
 * is the correctness baseline that pass-merge will be measured
 * against in the `effect-chain-gpu-pass-merge` cycle.
 *
 * Move-only (matches CPU EffectChain) — one chain per
 * ComposeSink / clip.
 */
#pragma once

#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace me::effect {

class GpuEffectChain {
public:
    GpuEffectChain() = default;
    ~GpuEffectChain() = default;

    GpuEffectChain(const GpuEffectChain&)            = delete;
    GpuEffectChain& operator=(const GpuEffectChain&) = delete;
    GpuEffectChain(GpuEffectChain&&) noexcept            = default;
    GpuEffectChain& operator=(GpuEffectChain&&) noexcept = default;

    /* Append an effect to the end of the chain. Chain takes
     * ownership. nullptr is a caller bug (asserted; silently
     * skipped in release). */
    void append(std::unique_ptr<GpuEffect> e) {
        assert(e && "GpuEffectChain::append: null GpuEffect");
        if (!e) return;
        effects_.push_back(std::move(e));
    }

    /* Submit the entire chain. See file-level doc for the
     * ping-pong scheme. `width` / `height` are the output canvas
     * dimensions; the chain sets each view's rect to (0, 0, w, h)
     * before calling the effect's submit so individual effects don't
     * need to re-discover dimensions (and so a Noop / placeholder
     * effect works correctly without needing to know its size). */
    void submit(bgfx::ViewId            first_view_id,
                std::uint16_t           width,
                std::uint16_t           height,
                bgfx::TextureHandle     src,
                bgfx::FrameBufferHandle dst,
                bgfx::FrameBufferHandle scratch_a,
                bgfx::FrameBufferHandle scratch_b) const {
        const std::size_t n = effects_.size();
        if (n == 0) return;

        if (n == 1) {
            bgfx::setViewRect(first_view_id, 0, 0, width, height);
            effects_[0]->submit(first_view_id, src, dst);
            return;
        }

        /* N ≥ 2: both scratches must be valid. */
        assert(bgfx::isValid(scratch_a) && bgfx::isValid(scratch_b)
               && "GpuEffectChain::submit: N≥2 requires two valid scratch framebuffers");

        bgfx::FrameBufferHandle prev_fb = BGFX_INVALID_HANDLE;
        for (std::size_t i = 0; i < n; ++i) {
            const bgfx::ViewId view = static_cast<bgfx::ViewId>(
                first_view_id + static_cast<bgfx::ViewId>(i));

            const bgfx::TextureHandle in_tex =
                (i == 0) ? src : bgfx::getTexture(prev_fb, 0);

            const bgfx::FrameBufferHandle out_fb =
                (i == n - 1)       ? dst
                : (i % 2 == 0)     ? scratch_a
                                   : scratch_b;

            bgfx::setViewRect(view, 0, 0, width, height);
            effects_[i]->submit(view, in_tex, out_fb);
            prev_fb = out_fb;
        }
    }

    std::size_t size() const noexcept { return effects_.size(); }
    bool        empty() const noexcept { return effects_.empty(); }

    const char* kind_at(std::size_t i) const noexcept {
        if (i >= effects_.size()) return nullptr;
        return effects_[i]->kind();
    }

    /* Fuse adjacent compatible effects into combined single-pass
     * shaders. Phase-1 catalog: two adjacent ColorCorrectEffects
     * collapse into a single FusedColorCorrectEffect (see
     * `fused_color_correct_effect.hpp`). Non-compatible neighbors
     * survive unchanged; fuse does not cascade (3 consecutive CCs
     * collapse to 1 fused + 1 unchanged, left-to-right greedy) —
     * wider fusion windows are a mechanical extension when visual
     * workloads warrant.
     *
     * Must run on the bgfx API thread: fused effect construction
     * creates bgfx handles. Typical caller pattern:
     *   backend->submit_on_render_thread([&] { chain.compile(); });
     *
     * Idempotent — calling compile twice on an already-compiled
     * chain does not double-fuse (the fused effect is a different
     * type and won't match the CC+CC detector). */
    void compile();

private:
    std::vector<std::unique_ptr<GpuEffect>> effects_;
};

}  // namespace me::effect
