/*
 * test_audio_resample_kernel — pins the AudioResample kernel ABI.
 *
 * Source-level resample math is covered by test_audio_resample
 * (drives me::audio::resample_to directly). This file pins:
 *   1. AVFrame in → AVFrame out at requested target_rate / channels.
 *   2. Schema declares target_rate, target_fmt, target_channels params.
 *   3. Missing target_rate → ME_E_INVALID_ARG.
 */
#include <doctest/doctest.h>

#include "audio/resample_kernel.hpp"
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
#include <mutex>
#include <span>

using namespace me;

namespace {

void register_once() {
    static std::once_flag once;
    std::call_once(once, []() { audio::register_resample_kind(); });
}

/* Build a synthetic stereo s16 AVFrame at 48 kHz with N samples. */
std::shared_ptr<AVFrame> make_audio_frame(int rate, int channels, int n_samples,
                                            AVSampleFormat fmt) {
    AVFrame* f = av_frame_alloc();
    f->format      = fmt;
    f->sample_rate = rate;
    f->nb_samples  = n_samples;
    av_channel_layout_default(&f->ch_layout, channels);
    av_frame_get_buffer(f, 0);
    /* Zeroed sample data is fine for ABI tests. */
    av_samples_set_silence(f->extended_data, 0, n_samples, channels, fmt);
    return std::shared_ptr<AVFrame>(f, [](AVFrame* x) {
        if (x) av_frame_free(&x);
    });
}

}  // namespace

TEST_CASE("AudioResample: 48kHz s16 → 44.1kHz fltp produces fewer samples") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::AudioResample,
                                     task::Affinity::Cpu);
    REQUIRE(fn);

    auto src_frame = make_audio_frame(/*rate=*/48000, /*channels=*/2,
                                       /*n_samples=*/1024,
                                       /*fmt=*/AV_SAMPLE_FMT_S16);
    graph::InputValue in;
    in.v = src_frame;

    graph::Properties props;
    props["target_rate"].v     = int64_t(44100);
    props["target_fmt"].v      = int64_t(AV_SAMPLE_FMT_FLTP);
    props["target_channels"].v = int64_t(2);

    graph::OutputSlot out;
    task::TaskContext ctx{};
    REQUIRE(fn(ctx, props,
               std::span<const graph::InputValue>{&in, 1},
               std::span<graph::OutputSlot>      {&out, 1}) == ME_OK);

    auto* dst_pp = std::get_if<std::shared_ptr<AVFrame>>(&out.v);
    REQUIRE(dst_pp);
    REQUIRE(*dst_pp);
    AVFrame* dst = (*dst_pp).get();
    CHECK(dst->sample_rate == 44100);
    CHECK(dst->format      == AV_SAMPLE_FMT_FLTP);
    CHECK(dst->ch_layout.nb_channels == 2);
    /* 48k → 44.1k yields fewer samples (≈940 for 1024 in). */
    CHECK(dst->nb_samples > 0);
    CHECK(dst->nb_samples < 1024);
}

TEST_CASE("AudioResample: missing target_rate → ME_E_INVALID_ARG") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::AudioResample,
                                     task::Affinity::Cpu);
    auto src_frame = make_audio_frame(48000, 2, 256, AV_SAMPLE_FMT_S16);
    graph::InputValue in; in.v = src_frame;
    graph::Properties props;
    /* target_rate omitted */
    graph::OutputSlot out;
    task::TaskContext ctx{};
    CHECK(fn(ctx, props,
             std::span<const graph::InputValue>{&in, 1},
             std::span<graph::OutputSlot>      {&out, 1}) == ME_E_INVALID_ARG);
}

TEST_CASE("AudioResample: schema") {
    register_once();
    const auto* info = task::lookup(task::TaskKindId::AudioResample);
    REQUIRE(info);
    REQUIRE(info->input_schema.size() == 1);
    CHECK(info->input_schema[0].type == graph::TypeId::AvFrameHandle);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->output_schema[0].type == graph::TypeId::AvFrameHandle);
    CHECK(info->time_invariant == true);
    CHECK(info->cacheable == true);
    REQUIRE(info->param_schema.size() == 3);
}
