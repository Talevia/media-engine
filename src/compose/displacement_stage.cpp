/* displacement_stage impl. Decodes the displacement texture via
 * sticker_decoder (same path as face_sticker / face_mosaic), then
 * dispatches to apply_displacement_inplace. */
#include "compose/displacement_stage.hpp"

#include "compose/displacement_kernel.hpp"
#include "compose/sticker_decoder.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::compose {

namespace {

std::string read_string_prop(const graph::Properties& props,
                              const std::string&       key) {
    auto it = props.find(key);
    if (it == props.end()) return {};
    if (const auto* p = std::get_if<std::string>(&it->second.v)) return *p;
    return {};
}

double read_double_prop(const graph::Properties& props,
                        const std::string&       key,
                        double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

me_status_t displacement_kernel_fn(task::TaskContext&,
                                    const graph::Properties&           props,
                                    std::span<const graph::InputValue> inputs,
                                    std::span<graph::OutputSlot>       outs) {
    if (inputs.size() < 1) return ME_E_INVALID_ARG;
    if (outs.size()   < 1) return ME_E_INVALID_ARG;

    auto* in_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!in_pp || !*in_pp) return ME_E_INVALID_ARG;
    const auto& in = **in_pp;
    if (in.width <= 0 || in.height <= 0 ||
        in.stride < static_cast<std::size_t>(in.width) * 4) {
        return ME_E_INVALID_ARG;
    }

    const std::string texture_uri = read_string_prop(props, "texture_uri");
    const float       strength_x  = static_cast<float>(read_double_prop(props, "strength_x", 0.0));
    const float       strength_y  = static_cast<float>(read_double_prop(props, "strength_y", 0.0));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;

    /* Empty URI OR (strength_x == 0 AND strength_y == 0) → identity. */
    if (texture_uri.empty() || (strength_x == 0.0f && strength_y == 0.0f)) {
        outs[0].v = std::move(dst);
        return ME_OK;
    }

    StickerImage tex;
    std::string  err;
    const me_status_t ds = decode_sticker_to_rgba8(texture_uri, &tex, &err);
    if (ds != ME_OK) return ds;

    const me_status_t s = apply_displacement_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride,
        tex.pixels.data(), tex.width, tex.height,
        strength_x, strength_y);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_displacement_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderDisplacement,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = displacement_kernel_fn,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "texture_uri", .type = graph::TypeId::String},
            {.name = "strength_x",  .type = graph::TypeId::Float64},
            {.name = "strength_y",  .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
