/* face_mosaic_stage impl. See header for the contract.
 *
 * Kernel chain inside the call:
 *   1. Read RGBA8 input + properties.
 *   2. resolve_landmark_bboxes_from_file (JSON fixture → bbox span).
 *   3. apply_face_mosaic_inplace (in-place pixelate / blur).
 *
 * Allocates a fresh RgbaFrameData for the output and deep-copies
 * the input bytes before the in-place pass — graph values are
 * immutable, can't mutate the input buffer.
 */
#include "compose/face_mosaic_stage.hpp"

#include "compose/bbox.hpp"
#include "compose/face_mosaic_kernel.hpp"
#include "compose/landmark_resolver.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/effect_params/face_mosaic.hpp"

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

me_status_t face_mosaic_kernel(task::TaskContext&,
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
    const std::string  landmark_uri = read_string_prop(props, "landmark_asset_uri");
    const me_rational_t frame_t     = read_rational_props(props, "frame_t_num", "frame_t_den");
    const std::int64_t block_size   = read_int64_prop(props, "block_size_px", 16);
    const std::int64_t mosaic_kind  = read_int64_prop(props, "mosaic_kind",   0);

    /* --- Resolve landmark bboxes ------------------------------ */
    std::vector<Bbox> bboxes;
    std::string       err;
    if (!landmark_uri.empty()) {
        const me_status_t s = resolve_landmark_bboxes_from_file(
            landmark_uri, frame_t, &bboxes, &err);
        if (s != ME_OK) return s;
    }

    /* --- Allocate output, copy input bytes, in-place mosaic --- */
    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy — graph values are immutable */

    me::FaceMosaicEffectParams p;
    p.landmark.asset_id = landmark_uri;  /* documentation-only */
    p.block_size_px     = static_cast<int>(block_size);
    p.kind = (mosaic_kind == 1)
                ? me::FaceMosaicEffectParams::Kind::Blur
                : me::FaceMosaicEffectParams::Kind::Pixelate;

    const me_status_t s = apply_face_mosaic_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride,
        p,
        std::span<const Bbox>(bboxes.data(), bboxes.size()));
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_face_mosaic_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderFaceMosaic,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        /* Same reasoning as face_sticker: resolver samples per frame_t
         * so a fixed input frame at different times can yield
         * different bboxes → different output. */
        .time_invariant = false,
        .kernel         = face_mosaic_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "landmark_asset_uri", .type = graph::TypeId::String},
            {.name = "frame_t_num",        .type = graph::TypeId::Int64},
            {.name = "frame_t_den",        .type = graph::TypeId::Int64},
            {.name = "block_size_px",      .type = graph::TypeId::Int64},
            {.name = "mosaic_kind",        .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
