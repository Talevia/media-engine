/* vignette_stage impl. See header for the contract. */
#include "compose/vignette_stage.hpp"

#include "compose/vignette_kernel.hpp"
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

double read_double_prop(const graph::Properties& props,
                         const std::string&       key,
                         double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

me_status_t vignette_kernel(task::TaskContext&,
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

    me::VignetteEffectParams p;
    p.radius    = static_cast<float>(read_double_prop(props, "radius",    0.5));
    p.softness  = static_cast<float>(read_double_prop(props, "softness",  0.3));
    p.intensity = static_cast<float>(read_double_prop(props, "intensity", 0.0));
    p.center_x  = static_cast<float>(read_double_prop(props, "center_x",  0.5));
    p.center_y  = static_cast<float>(read_double_prop(props, "center_y",  0.5));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy */

    const me_status_t s = apply_vignette_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_vignette_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderVignette,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = vignette_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "radius",    .type = graph::TypeId::Float64},
            {.name = "softness",  .type = graph::TypeId::Float64},
            {.name = "intensity", .type = graph::TypeId::Float64},
            {.name = "center_x",  .type = graph::TypeId::Float64},
            {.name = "center_y",  .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
