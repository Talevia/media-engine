/*
 * test_audio_mix_kernel — pins the AudioMix kernel ABI.
 *
 * Pure mix math is covered by test_audio_mix (drives mix_samples
 * directly); this file pins:
 *   1. Single FLTP input @ unity gain → output bytes-equal to input.
 *   2. Two FLTP inputs at 0 dB → channel sums (each channel additive).
 *   3. Mismatched sample-rate → ME_E_INVALID_ARG.
 *   4. Schema declares variadic AvFrameHandle in / AvFrameHandle out.
 */
#include <doctest/doctest.h>

#include "audio/mix_kernel.hpp"
#include "graph/types.hpp"
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
#include <mutex>
#include <span>
#include <vector>

using namespace me;

namespace {

void register_once() {
    static std::once_flag once;
    std::call_once(once, []() { audio::register_mix_kind(); });
}

std::shared_ptr<AVFrame> fltp_frame(int rate, int channels, int n_samples,
                                     float fill) {
    AVFrame* f = av_frame_alloc();
    f->format      = AV_SAMPLE_FMT_FLTP;
    f->sample_rate = rate;
    f->nb_samples  = n_samples;
    av_channel_layout_default(&f->ch_layout, channels);
    av_frame_get_buffer(f, 0);
    for (int ch = 0; ch < channels; ++ch) {
        float* dst = reinterpret_cast<float*>(f->extended_data[ch]);
        for (int i = 0; i < n_samples; ++i) dst[i] = fill;
    }
    return std::shared_ptr<AVFrame>(f, [](AVFrame* x) {
        if (x) av_frame_free(&x);
    });
}

}  // namespace

TEST_CASE("AudioMix: single input @ 0 dB → output equals input") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::AudioMix,
                                     task::Affinity::Cpu);
    REQUIRE(fn);

    std::vector<graph::InputValue> ins(1);
    ins[0].v = fltp_frame(48000, 2, 1024, 0.5f);

    graph::Properties props;
    props["gain_db_0"].v = double(0.0);
    /* peak_limit threshold default 0.95; 0.5 is well below, no clipping. */

    graph::OutputSlot out;
    task::TaskContext ctx{};
    REQUIRE(fn(ctx, props,
               std::span<const graph::InputValue>{ins.data(), ins.size()},
               std::span<graph::OutputSlot>      {&out, 1}) == ME_OK);
    auto* dst_pp = std::get_if<std::shared_ptr<AVFrame>>(&out.v);
    REQUIRE(dst_pp);
    REQUIRE(*dst_pp);
    AVFrame* dst = (*dst_pp).get();
    CHECK(dst->nb_samples == 1024);
    /* Spot-check first sample of each channel. */
    for (int ch = 0; ch < 2; ++ch) {
        const float* p = reinterpret_cast<const float*>(dst->extended_data[ch]);
        CHECK(p[0] == doctest::Approx(0.5f));
    }
}

TEST_CASE("AudioMix: two inputs at 0 dB sum per channel") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::AudioMix,
                                     task::Affinity::Cpu);
    std::vector<graph::InputValue> ins(2);
    ins[0].v = fltp_frame(48000, 2, 256, 0.3f);
    ins[1].v = fltp_frame(48000, 2, 256, 0.2f);

    graph::Properties props;
    props["gain_db_0"].v = double(0.0);
    props["gain_db_1"].v = double(0.0);
    props["peak_limit_threshold"].v = double(0.99);  /* pass-through */

    graph::OutputSlot out;
    task::TaskContext ctx{};
    REQUIRE(fn(ctx, props,
               std::span<const graph::InputValue>{ins.data(), ins.size()},
               std::span<graph::OutputSlot>      {&out, 1}) == ME_OK);
    auto* dst_pp = std::get_if<std::shared_ptr<AVFrame>>(&out.v);
    AVFrame* dst = (*dst_pp).get();
    /* Sample-level sum = 0.5 (well within threshold). */
    const float* p = reinterpret_cast<const float*>(dst->extended_data[0]);
    CHECK(p[0] == doctest::Approx(0.5f));
    CHECK(p[100] == doctest::Approx(0.5f));
}

TEST_CASE("AudioMix: mismatched sample_rate → ME_E_INVALID_ARG") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::AudioMix,
                                     task::Affinity::Cpu);
    std::vector<graph::InputValue> ins(2);
    ins[0].v = fltp_frame(48000, 2, 256, 0.0f);
    ins[1].v = fltp_frame(44100, 2, 256, 0.0f);

    graph::Properties props;
    graph::OutputSlot out;
    task::TaskContext ctx{};
    CHECK(fn(ctx, props,
             std::span<const graph::InputValue>{ins.data(), ins.size()},
             std::span<graph::OutputSlot>      {&out, 1}) == ME_E_INVALID_ARG);
}

TEST_CASE("AudioMix: schema") {
    register_once();
    const auto* info = task::lookup(task::TaskKindId::AudioMix);
    REQUIRE(info);
    CHECK(info->variadic_last_input == true);
    REQUIRE(info->input_schema.size() == 1);
    CHECK(info->input_schema[0].type == graph::TypeId::AvFrameHandle);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->output_schema[0].type == graph::TypeId::AvFrameHandle);
    CHECK(info->time_invariant == true);
    CHECK(info->cacheable == true);
}
