#include "audio/mix_kernel.hpp"

#include "audio/mix.hpp"
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
#include <vector>

namespace me::audio {

namespace {

double prop_double_or(const graph::Properties& props,
                      const std::string&       key,
                      double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

me_status_t mix_kernel(task::TaskContext&,
                        const graph::Properties&           props,
                        std::span<const graph::InputValue> inputs,
                        std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;

    /* Pull and validate inputs. */
    std::vector<AVFrame*> frames;
    frames.reserve(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto* pp = std::get_if<std::shared_ptr<AVFrame>>(&inputs[i].v);
        if (!pp || !*pp) return ME_E_INVALID_ARG;
        frames.push_back(pp->get());
    }

    AVFrame* ref = frames[0];
    if (ref->format != AV_SAMPLE_FMT_FLTP) return ME_E_INVALID_ARG;
    if (ref->sample_rate <= 0 || ref->nb_samples <= 0) return ME_E_INVALID_ARG;
    const int n_channels = ref->ch_layout.nb_channels;
    const int n_samples  = ref->nb_samples;
    if (n_channels <= 0) return ME_E_INVALID_ARG;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frames[i]->format      != ref->format)      return ME_E_INVALID_ARG;
        if (frames[i]->sample_rate != ref->sample_rate) return ME_E_INVALID_ARG;
        if (frames[i]->nb_samples  != ref->nb_samples)  return ME_E_INVALID_ARG;
        if (frames[i]->ch_layout.nb_channels != n_channels) return ME_E_INVALID_ARG;
    }

    /* Allocate output AVFrame matching reference params. */
    AVFrame* dst = av_frame_alloc();
    if (!dst) return ME_E_OUT_OF_MEMORY;
    dst->format      = ref->format;
    dst->sample_rate = ref->sample_rate;
    dst->nb_samples  = n_samples;
    if (av_channel_layout_copy(&dst->ch_layout, &ref->ch_layout) < 0) {
        av_frame_free(&dst);
        return ME_E_INTERNAL;
    }
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return ME_E_OUT_OF_MEMORY;
    }

    /* Pre-compute per-input gain in linear amplitude. */
    std::vector<float> gains(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const double db = prop_double_or(props,
                                         "gain_db_" + std::to_string(i),
                                         0.0);
        gains[i] = db_to_linear(static_cast<float>(db));
    }

    const float peak_thr = static_cast<float>(
        prop_double_or(props, "peak_limit_threshold", 0.95));

    /* Per-channel mix loop. FLTP stores channels in extended_data[ch]. */
    std::vector<const float*> chan_inputs(frames.size());
    for (int ch = 0; ch < n_channels; ++ch) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            chan_inputs[i] = reinterpret_cast<const float*>(frames[i]->extended_data[ch]);
        }
        float* out_ch = reinterpret_cast<float*>(dst->extended_data[ch]);
        mix_samples(chan_inputs.data(), gains.data(),
                    frames.size(),
                    static_cast<std::size_t>(n_samples),
                    out_ch);
        if (peak_thr > 0.0f) {
            peak_limiter(out_ch, static_cast<std::size_t>(n_samples), peak_thr);
        }
    }

    std::shared_ptr<AVFrame> shared(dst, [](AVFrame* f) {
        if (f) av_frame_free(&f);
    });
    outs[0].v = std::move(shared);
    return ME_OK;
}

}  // namespace

void register_mix_kind() {
    task::KindInfo info{
        .kind                = task::TaskKindId::AudioMix,
        .affinity            = task::Affinity::Cpu,
        .latency             = task::Latency::Short,
        .time_invariant      = true,
        .variadic_last_input = true,
        .kernel              = mix_kernel,
        .input_schema        = { {"track", graph::TypeId::AvFrameHandle} },
        .output_schema       = { {"mixed", graph::TypeId::AvFrameHandle} },
        .param_schema        = {
            {.name = "peak_limit_threshold", .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::audio
