#include "compose/convert_rgba8_kernel.hpp"

#include "compose/frame_convert.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <memory>
#include <span>
#include <utility>

namespace me::compose {

namespace {

me_status_t convert_rgba8_kernel(task::TaskContext&,
                                 const graph::Properties&,
                                 std::span<const graph::InputValue> inputs,
                                 std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* frame_pp = std::get_if<std::shared_ptr<AVFrame>>(&inputs[0].v);
    if (!frame_pp || !*frame_pp) return ME_E_INVALID_ARG;
    const AVFrame* fr = frame_pp->get();
    if (fr->width <= 0 || fr->height <= 0) return ME_E_INVALID_ARG;

    auto rgba = std::make_shared<graph::RgbaFrameData>();
    rgba->width  = fr->width;
    rgba->height = fr->height;
    rgba->stride = static_cast<std::size_t>(fr->width) * 4;

    me_status_t s = frame_to_rgba8(fr, rgba->rgba);
    if (s != ME_OK) return s;

    outs[0].v = std::move(rgba);
    return ME_OK;
}

}  // namespace

void register_convert_rgba8_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderConvertRgba8,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = convert_rgba8_kernel,
        .input_schema   = { {"frame", graph::TypeId::AvFrameHandle} },
        .output_schema  = { {"rgba",  graph::TypeId::RgbaFrame} },
        .param_schema   = {},
    };
    task::register_kind(info);
}

}  // namespace me::compose
