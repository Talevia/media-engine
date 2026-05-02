/* body_alpha_key_stage impl. See header for the contract.
 *
 * Kernel chain inside the call:
 *   1. Read RGBA8 input + properties.
 *   2. resolve_mask_alpha_from_file (JSON+base64 → alpha plane).
 *   3. apply_body_alpha_key_inplace (in-place alpha multiply).
 *
 * Allocates a fresh RgbaFrameData for the output and deep-copies
 * the input bytes before the in-place pass — graph values are
 * immutable.
 *
 * Mask-size mismatch handling: the kernel rejects mismatched
 * dimensions with ME_E_INVALID_ARG. The stage forwards that error
 * verbatim so a misconfigured timeline (mask sized differently
 * from the frame) surfaces a deterministic diagnostic rather than
 * a silent rescale. A future cycle can add a resampling resolver
 * if production workflows need different-sized masks; today the
 * fixture-mode resolver expects the host to provide masks already
 * sized to the frame.
 */
#include "compose/body_alpha_key_stage.hpp"

#include "compose/body_alpha_key_kernel.hpp"
#include "compose/mask_resolver.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/effect_params/body_alpha_key.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::compose {

namespace {

me_rational_t read_rational_props(const graph::Properties& props,
                                   const std::string&       num_key,
                                   const std::string&       den_key) {
    me_rational_t out{0, 1};
    auto n_it = props.find(num_key);
    auto d_it = props.find(den_key);
    if (n_it == props.end() || d_it == props.end()) return out;
    const auto* n_p = std::get_if<std::int64_t>(&n_it->second.v);
    const auto* d_p = std::get_if<std::int64_t>(&d_it->second.v);
    if (!n_p || !d_p) return out;
    out.num = *n_p;
    out.den = (*d_p > 0) ? *d_p : 1;
    return out;
}

std::int64_t read_int64_prop(const graph::Properties& props,
                              const std::string&       key,
                              std::int64_t             fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<std::int64_t>(&it->second.v)) return *p;
    return fallback;
}

std::string read_string_prop(const graph::Properties& props,
                              const std::string&       key) {
    auto it = props.find(key);
    if (it == props.end()) return {};
    if (const auto* p = std::get_if<std::string>(&it->second.v)) return *p;
    return {};
}

me_status_t body_alpha_key_kernel(task::TaskContext&,
                                   const graph::Properties&           props,
                                   std::span<const graph::InputValue> inputs,
                                   std::span<graph::OutputSlot>       outs) {
    if (inputs.size() < 1) return ME_E_INVALID_ARG;
    if (outs.size()   < 1) return ME_E_INVALID_ARG;

    /* --- Read input frame ------------------------------------- */
    auto* in_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!in_pp || !*in_pp) return ME_E_INVALID_ARG;
    const auto& in = **in_pp;
    if (in.width <= 0 || in.height <= 0 ||
        in.stride < static_cast<std::size_t>(in.width) * 4) {
        return ME_E_INVALID_ARG;
    }

    /* --- Read properties -------------------------------------- */
    const std::string  mask_uri    = read_string_prop(props, "mask_asset_uri");
    const me_rational_t frame_t    = read_rational_props(props, "frame_t_num", "frame_t_den");
    const std::int64_t feather_px  = read_int64_prop(props, "feather_radius_px", 0);
    const std::int64_t invert_flag = read_int64_prop(props, "invert", 0);

    /* --- Resolve mask alpha plane ----------------------------- */
    int                       mask_w = 0, mask_h = 0;
    std::vector<std::uint8_t> mask_bytes;
    std::string               err;
    if (!mask_uri.empty()) {
        const me_status_t s = resolve_mask_alpha_from_file(
            mask_uri, frame_t, &mask_w, &mask_h, &mask_bytes, &err);
        if (s != ME_OK) return s;
    }

    /* --- Allocate output, copy input bytes -------------------- */
    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy — graph values are immutable */

    me::BodyAlphaKeyEffectParams p;
    p.mask.asset_id      = mask_uri;  /* documentation-only */
    p.feather_radius_px  = static_cast<int>(feather_px);
    p.invert             = (invert_flag != 0);

    /* Empty mask (resolver returned 0×0) → kernel no-op pass-
     * through. The kernel itself accepts a null mask plane and
     * returns ME_OK without touching the frame. */
    const std::uint8_t* mask_ptr = mask_bytes.empty() ? nullptr : mask_bytes.data();
    const std::size_t   mask_stride = mask_bytes.empty()
                                          ? 0
                                          : static_cast<std::size_t>(mask_w);

    const me_status_t s = apply_body_alpha_key_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride,
        p,
        mask_ptr, mask_w, mask_h, mask_stride);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_body_alpha_key_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderBodyAlphaKey,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = false,
        .kernel         = body_alpha_key_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "mask_asset_uri",    .type = graph::TypeId::String},
            {.name = "frame_t_num",       .type = graph::TypeId::Int64},
            {.name = "frame_t_den",       .type = graph::TypeId::Int64},
            {.name = "feather_radius_px", .type = graph::TypeId::Int64},
            {.name = "invert",            .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
