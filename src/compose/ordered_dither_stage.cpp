/* ordered_dither_stage impl. */
#include "compose/ordered_dither_stage.hpp"

#include "compose/ordered_dither_kernel.hpp"
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

me_status_t ordered_dither_kernel(task::TaskContext&,
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

    me::OrderedDitherEffectParams p;
    p.matrix_size = static_cast<int>(read_int64_prop(props, "matrix_size", 4));
    p.levels      = static_cast<int>(read_int64_prop(props, "levels",      256));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;

    const me_status_t s = apply_ordered_dither_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_ordered_dither_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderOrderedDither,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = ordered_dither_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "matrix_size", .type = graph::TypeId::Int64},
            {.name = "levels",      .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
