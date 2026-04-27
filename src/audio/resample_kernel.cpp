#include "audio/resample_kernel.hpp"

#include "audio/resample.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::audio {

namespace {

int64_t prop_int_or(const graph::Properties& props,
                    const char*              key,
                    int64_t                  fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<int64_t>(&it->second.v)) return *p;
    return fallback;
}

me_status_t resample_kernel(task::TaskContext&,
                             const graph::Properties&           props,
                             std::span<const graph::InputValue> inputs,
                             std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* src_pp = std::get_if<std::shared_ptr<AVFrame>>(&inputs[0].v);
    if (!src_pp || !*src_pp) return ME_E_INVALID_ARG;
    AVFrame* src = src_pp->get();

    int64_t target_rate_i64 = 0;
    if (auto it = props.find("target_rate"); it != props.end()) {
        if (auto* p = std::get_if<int64_t>(&it->second.v)) target_rate_i64 = *p;
    }
    if (target_rate_i64 <= 0) return ME_E_INVALID_ARG;

    const int64_t target_fmt_i64 =
        prop_int_or(props, "target_fmt", static_cast<int64_t>(AV_SAMPLE_FMT_FLTP));
    const int64_t target_channels =
        prop_int_or(props, "target_channels", 2);
    if (target_channels <= 0) return ME_E_INVALID_ARG;

    AVChannelLayout dst_layout{};
    av_channel_layout_default(&dst_layout, static_cast<int>(target_channels));

    AVFrame* out_frame = nullptr;
    std::string err;
    me_status_t s = resample_to(
        src,
        static_cast<int>(target_rate_i64),
        static_cast<AVSampleFormat>(target_fmt_i64),
        dst_layout,
        &out_frame,
        &err);

    av_channel_layout_uninit(&dst_layout);
    if (s != ME_OK) return s;
    if (!out_frame) return ME_E_INTERNAL;

    /* Wrap into shared_ptr<AVFrame> with libav-aware deleter so the
     * graph value carries proper lifetime. */
    std::shared_ptr<AVFrame> shared(out_frame, [](AVFrame* f) {
        if (f) av_frame_free(&f);
    });
    outs[0].v = std::move(shared);
    return ME_OK;
}

}  // namespace

void register_resample_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::AudioResample,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = resample_kernel,
        .input_schema   = { {"src", graph::TypeId::AvFrameHandle} },
        .output_schema  = { {"dst", graph::TypeId::AvFrameHandle} },
        .param_schema   = {
            {.name = "target_rate",     .type = graph::TypeId::Int64},
            {.name = "target_fmt",      .type = graph::TypeId::Int64},
            {.name = "target_channels", .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::audio
