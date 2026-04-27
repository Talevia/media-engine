#include "compose/cross_dissolve_kernel.hpp"

#include "compose/cross_dissolve.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <span>
#include <utility>

namespace me::compose {

namespace {

me_status_t cross_dissolve_kernel(task::TaskContext&,
                                   const graph::Properties&           props,
                                   std::span<const graph::InputValue> inputs,
                                   std::span<graph::OutputSlot>       outs) {
    if (inputs.size() < 2) return ME_E_INVALID_ARG;

    auto* from_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    auto* to_pp   = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[1].v);
    if (!from_pp || !*from_pp || !to_pp || !*to_pp) return ME_E_INVALID_ARG;

    const auto& from = **from_pp;
    const auto& to   = **to_pp;
    if (from.width != to.width ||
        from.height != to.height ||
        from.stride != to.stride) {
        return ME_E_INVALID_ARG;
    }

    auto pp = props.find("progress");
    if (pp == props.end()) return ME_E_INVALID_ARG;
    const auto* prog_p = std::get_if<double>(&pp->second.v);
    if (!prog_p) return ME_E_INVALID_ARG;
    const float progress = static_cast<float>(*prog_p);

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = from.width;
    dst->height = from.height;
    dst->stride = from.stride;
    dst->rgba.assign(from.rgba.size(), 0);

    cross_dissolve(dst->rgba.data(),
                   from.rgba.data(),
                   to.rgba.data(),
                   from.width, from.height, from.stride,
                   progress);

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_cross_dissolve_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderCrossDissolve,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = cross_dissolve_kernel,
        .input_schema   = {
            {"from", graph::TypeId::RgbaFrame},
            {"to",   graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "progress", .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
