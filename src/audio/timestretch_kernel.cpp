#include "audio/timestretch_kernel.hpp"

#include "audio/tempo.hpp"
#include "graph/types.hpp"
#include "resource/stateful_pool.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace me::audio {

namespace {

double prop_double_or(const graph::Properties& props,
                      const char*              key,
                      double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

int64_t prop_int_or(const graph::Properties& props,
                    const char*              key,
                    int64_t                  fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<int64_t>(&it->second.v)) return *p;
    return fallback;
}

/* Convert FLTP (planar float, channels in extended_data[]) → interleaved.
 * Output buffer must be at least n_samples * channels floats. */
void fltp_to_interleaved(const AVFrame* src,
                          float*         dst,
                          int            n_samples,
                          int            channels) {
    for (int i = 0; i < n_samples; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const float* plane = reinterpret_cast<const float*>(src->extended_data[ch]);
            dst[i * channels + ch] = plane[i];
        }
    }
}

/* Inverse: interleaved → FLTP. */
void interleaved_to_fltp(const float* src,
                          AVFrame*     dst,
                          int          n_samples,
                          int          channels) {
    for (int i = 0; i < n_samples; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            float* plane = reinterpret_cast<float*>(dst->extended_data[ch]);
            plane[i] = src[i * channels + ch];
        }
    }
}

me_status_t timestretch_kernel(task::TaskContext&                 ctx,
                                const graph::Properties&           props,
                                std::span<const graph::InputValue> inputs,
                                std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* src_pp = std::get_if<std::shared_ptr<AVFrame>>(&inputs[0].v);
    if (!src_pp || !*src_pp) return ME_E_INVALID_ARG;
    AVFrame* src = src_pp->get();
    if (src->format != AV_SAMPLE_FMT_FLTP) return ME_E_INVALID_ARG;
    if (src->sample_rate <= 0 || src->nb_samples <= 0) return ME_E_INVALID_ARG;
    const int channels  = src->ch_layout.nb_channels;
    const int n_samples = src->nb_samples;
    if (channels <= 0) return ME_E_INVALID_ARG;

    const double tempo = prop_double_or(props, "tempo", 1.0);
    if (tempo <= 0.0) return ME_E_INVALID_ARG;
    const int64_t instance_key = prop_int_or(props, "instance_key", 0);

    /* Borrow or build a TempoStretcher. The pool may be null in
     * test contexts or when ME_WITH_SOUNDTOUCH is off + the engine
     * elected not to allocate it; either way fall back to fresh-
     * per-call. */
    using TempoPool = resource::StatefulResourcePool<TempoStretcher>;
    TempoPool* pool = ctx.tempo_pool;

    typename TempoPool::Handle handle;
    std::unique_ptr<TempoStretcher> owned_fallback;
    TempoStretcher* st = nullptr;

    if (pool) {
        handle = pool->borrow(static_cast<uint64_t>(instance_key));
        if (handle) {
            /* Validate sample_rate + channels match. SoundTouch can't
             * re-init mid-life — if config changed, evict + rebuild. */
            if (handle->sample_rate() != src->sample_rate ||
                handle->channels()    != channels) {
                handle = {};   /* drops back into pool, but we'll evict next */
                pool->evict(static_cast<uint64_t>(instance_key));
            }
        }
        if (!handle) {
            auto fresh = std::make_unique<TempoStretcher>(src->sample_rate, channels);
            handle = pool->adopt(static_cast<uint64_t>(instance_key), std::move(fresh));
        }
        st = handle.get();
    } else {
        owned_fallback = std::make_unique<TempoStretcher>(src->sample_rate, channels);
        st = owned_fallback.get();
    }
    if (!st) return ME_E_INTERNAL;

    st->set_tempo(tempo);

    /* Convert to interleaved + feed SoundTouch. */
    std::vector<float> interleaved_in(static_cast<std::size_t>(n_samples) * channels);
    fltp_to_interleaved(src, interleaved_in.data(), n_samples, channels);
    st->put_samples(interleaved_in.data(),
                    static_cast<std::size_t>(n_samples));

    /* Pull whatever's available. SoundTouch's output for tempo=1.0
     * is bounded by input; for tempo<1 (slower) it can be larger; we
     * give it generous headroom (4× input) to avoid losing samples
     * on fast-tempo lookahead. */
    const std::size_t out_cap = static_cast<std::size_t>(n_samples) * 4u;
    std::vector<float> interleaved_out(out_cap * channels);
    std::size_t produced = 0;
    while (produced < out_cap) {
        const std::size_t got = st->receive_samples(
            interleaved_out.data() + produced * channels,
            out_cap - produced);
        if (got == 0) break;
        produced += got;
    }

    AVFrame* dst = av_frame_alloc();
    if (!dst) return ME_E_OUT_OF_MEMORY;
    dst->format      = AV_SAMPLE_FMT_FLTP;
    dst->sample_rate = src->sample_rate;
    if (av_channel_layout_copy(&dst->ch_layout, &src->ch_layout) < 0) {
        av_frame_free(&dst);
        return ME_E_INTERNAL;
    }
    dst->nb_samples = static_cast<int>(produced);
    if (produced == 0) {
        /* SoundTouch hasn't accumulated enough yet — emit a zero-
         * sample AVFrame; downstream consumers concat across chunks. */
        std::shared_ptr<AVFrame> shared(dst, [](AVFrame* f) {
            if (f) av_frame_free(&f);
        });
        outs[0].v = std::move(shared);
        return ME_OK;
    }
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return ME_E_OUT_OF_MEMORY;
    }
    interleaved_to_fltp(interleaved_out.data(), dst,
                         static_cast<int>(produced), channels);

    std::shared_ptr<AVFrame> shared(dst, [](AVFrame* f) {
        if (f) av_frame_free(&f);
    });
    outs[0].v = std::move(shared);
    return ME_OK;
}

}  // namespace

void register_timestretch_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::AudioTimestretch,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Medium,
        .time_invariant = false,
        .cacheable      = false,    /* output depends on borrowed instance state */
        .kernel         = timestretch_kernel,
        .input_schema   = { {"src", graph::TypeId::AvFrameHandle} },
        .output_schema  = { {"dst", graph::TypeId::AvFrameHandle} },
        .param_schema   = {
            {.name = "tempo",        .type = graph::TypeId::Float64},
            {.name = "instance_key", .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::audio
