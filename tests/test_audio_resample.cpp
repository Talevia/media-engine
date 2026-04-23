/*
 * test_audio_resample — tripwire for me::audio::resample_to.
 *
 * Covers the dimensional contract (output rate / format / channel
 * layout match the request), sample-count scaling (2× rate → ~2×
 * samples), and failure modes (null args, non-positive rate).
 * Numerical pixel-value assertions are intentionally narrow — real
 * resample quality is libswresample's concern; we just verify the
 * wrapper plumbs it correctly.
 */
#include <doctest/doctest.h>

#include "audio/resample.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

using me::audio::resample_to;

namespace {

struct FrameGuard {
    AVFrame* f = nullptr;
    ~FrameGuard() { if (f) av_frame_free(&f); }
};

/* Build a synthetic decoded AVFrame: S16 interleaved, mono, given
 * sample rate + sample count, filled with a simple ramp so we can
 * check the convert didn't silently zero. */
AVFrame* make_s16_mono(int sample_rate, int nb_samples) {
    AVFrame* f = av_frame_alloc();
    REQUIRE(f != nullptr);
    f->format      = AV_SAMPLE_FMT_S16;
    f->sample_rate = sample_rate;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(av_channel_layout_copy(&f->ch_layout, &mono) == 0);
    f->nb_samples  = nb_samples;
    REQUIRE(av_frame_get_buffer(f, 0) == 0);
    int16_t* p = reinterpret_cast<int16_t*>(f->data[0]);
    for (int i = 0; i < nb_samples; ++i) p[i] = static_cast<int16_t>((i * 37) % 30000);
    return f;
}

}  // namespace

TEST_CASE("resample_to: null src returns ME_E_INVALID_ARG") {
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    std::string err;
    CHECK(resample_to(nullptr, 48000, AV_SAMPLE_FMT_FLT, mono, &out, &err) == ME_E_INVALID_ARG);
    CHECK(out == nullptr);
}

TEST_CASE("resample_to: null out pointer returns ME_E_INVALID_ARG") {
    FrameGuard g{make_s16_mono(48000, 128)};
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    CHECK(resample_to(g.f, 48000, AV_SAMPLE_FMT_FLT, mono, nullptr, nullptr)
          == ME_E_INVALID_ARG);
}

TEST_CASE("resample_to: non-positive dst_rate returns ME_E_INVALID_ARG") {
    FrameGuard g{make_s16_mono(48000, 128)};
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    CHECK(resample_to(g.f, 0,  AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_E_INVALID_ARG);
    CHECK(resample_to(g.f, -1, AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("resample_to: identity (same params) produces frame with same params") {
    FrameGuard src{make_s16_mono(48000, 128)};
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(resample_to(src.f, 48000, AV_SAMPLE_FMT_S16, mono, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    CHECK(out->sample_rate == 48000);
    CHECK(out->format == AV_SAMPLE_FMT_S16);
    CHECK(out->ch_layout.nb_channels == 1);
    /* nb_samples may shrink by 1-2 due to resampler latency on the first
     * flush, so accept >= 120 of the 128 input samples. */
    CHECK(out->nb_samples >= 120);
    CHECK(out->nb_samples <= 128);
}

TEST_CASE("resample_to: 48000 → 96000 upsampling produces ~2× sample count") {
    FrameGuard src{make_s16_mono(48000, 256)};
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(resample_to(src.f, 96000, AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    CHECK(out->sample_rate == 96000);
    CHECK(out->format == AV_SAMPLE_FMT_FLT);
    /* Expect ~2× samples; allow a wide tolerance for resampler
     * transient / filter-length overhead. */
    CHECK(out->nb_samples >= 400);
    CHECK(out->nb_samples <= 600);
}

TEST_CASE("resample_to: 48000 → 24000 downsampling produces ~half sample count") {
    FrameGuard src{make_s16_mono(48000, 256)};
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(resample_to(src.f, 24000, AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    CHECK(out->sample_rate == 24000);
    CHECK(out->nb_samples >= 100);
    CHECK(out->nb_samples <= 160);
}

TEST_CASE("resample_to: mono → stereo upmix produces 2 channels") {
    FrameGuard src{make_s16_mono(48000, 128)};
    AVFrame* out = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    REQUIRE(resample_to(src.f, 48000, AV_SAMPLE_FMT_FLT, stereo, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    CHECK(out->ch_layout.nb_channels == 2);
}

TEST_CASE("resample_to: S16 → FLT format change produces float-sized output") {
    FrameGuard src{make_s16_mono(48000, 64)};
    AVFrame* out = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(resample_to(src.f, 48000, AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    CHECK(out->format == AV_SAMPLE_FMT_FLT);
    /* Spot-check: reading as float should be finite + in a reasonable
     * amplitude range (input S16 values are 0..30000 so scaled to
     * float they are 0 .. ~0.91). */
    const float* p = reinterpret_cast<const float*>(out->data[0]);
    for (int i = 0; i < out->nb_samples; ++i) {
        CHECK(p[i] >= -1.01f);
        CHECK(p[i] <=  1.01f);
    }
}

TEST_CASE("resample_to: empty src (nb_samples=0) returns empty dst without crash") {
    AVFrame* src = av_frame_alloc();
    src->format = AV_SAMPLE_FMT_S16;
    src->sample_rate = 48000;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    REQUIRE(av_channel_layout_copy(&src->ch_layout, &mono) == 0);
    src->nb_samples = 0;
    FrameGuard src_g{src};

    AVFrame* out = nullptr;
    CHECK(resample_to(src, 48000, AV_SAMPLE_FMT_FLT, mono, &out, nullptr) == ME_OK);
    FrameGuard out_g{out};
    REQUIRE(out != nullptr);
    CHECK(out->nb_samples == 0);
}
