/* tilt_shift_stage impl. */
#include "compose/tilt_shift_stage.hpp"

#include "compose/tilt_shift_kernel.hpp"
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

std::int64_t read_int64_prop(const graph::Properties& props,
                              const std::string&       key,
                              std::int64_t             fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<std::int64_t>(&it->second.v)) return *p;
    return fallback;
}

double read_double_prop(const graph::Properties& props,
                        const std::string&       key,
                        double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

me_status_t tilt_shift_kernel(task::TaskContext&,
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

    me::TiltShiftEffectParams p;
    p.focal_y_min     = static_cast<float>(read_double_prop(props, "focal_y_min",     0.4));
    p.focal_y_max     = static_cast<float>(read_double_prop(props, "focal_y_max",     0.6));
    p.edge_softness   = static_cast<float>(read_double_prop(props, "edge_softness",   0.2));
    p.max_blur_radius = static_cast<int>  (read_int64_prop (props, "max_blur_radius", 0));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;

    const me_status_t s = apply_tilt_shift_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_tilt_shift_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderTiltShift,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = tilt_shift_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "focal_y_min",     .type = graph::TypeId::Float64},
            {.name = "focal_y_max",     .type = graph::TypeId::Float64},
            {.name = "edge_softness",   .type = graph::TypeId::Float64},
            {.name = "max_blur_radius", .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
