/*
 * test_audio_mixer — per-class contract for me::audio::AudioMixer.
 *
 * Exercises the full N-track mixer path: multiple AudioTrackFeed
 * instances (all reading the silent AAC from the with-audio
 * fixture) summed + peak-limited → mixed AVFrame. Silent inputs
 * mean the output is also silent regardless of track count / gains,
 * which pins the contract without needing complex reference
 * waveforms.
 */
#include <doctest/doctest.h>

#include "audio/mixer.hpp"
#include "audio/track_feed.hpp"
#include "io/demux_context.hpp"
#include "resource/codec_pool.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO
#define ME_TEST_FIXTURE_MP4_WITH_AUDIO ""
#endif

namespace {

std::shared_ptr<me::io::DemuxContext> open_demux(const std::string& path) {
    auto d = std::make_shared<me::io::DemuxContext>();
    if (avformat_open_input(&d->fmt, path.c_str(), nullptr, nullptr) < 0) return {};
    if (avformat_find_stream_info(d->fmt, nullptr) < 0) return {};
    d->uri = path;
    return d;
}

struct LayoutGuard {
    AVChannelLayout l{};
    LayoutGuard(AVChannelLayout src) { av_channel_layout_copy(&l, &src); }
    ~LayoutGuard() { av_channel_layout_uninit(&l); }
};

me::audio::AudioMixerConfig make_cfg(const AVChannelLayout& mono) {
    me::audio::AudioMixerConfig cfg;
    cfg.target_rate    = 48000;
    cfg.target_fmt     = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_copy(&cfg.target_ch_layout, &mono);
    cfg.frame_size     = 1024;
    cfg.peak_threshold = 0.95f;
    return cfg;
}

}  // namespace

TEST_CASE("AudioMixer ctor: non-FLTP target rejected") {
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    cfg.target_fmt = AV_SAMPLE_FMT_S16;
    std::string err;
    me::audio::AudioMixer m(cfg, &err);
    CHECK_FALSE(m.ok());
    CHECK(err.find("FLTP") != std::string::npos);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer ctor: zero frame_size rejected") {
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    cfg.frame_size = 0;
    std::string err;
    me::audio::AudioMixer m(cfg, &err);
    CHECK_FALSE(m.ok());
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer::pull_next_mixed_frame: no tracks added returns ME_E_INVALID_ARG") {
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    std::string err;
    me::audio::AudioMixer m(cfg, &err);
    REQUIRE(m.ok());
    AVFrame* f = nullptr;
    CHECK(m.pull_next_mixed_frame(&f, &err) == ME_E_INVALID_ARG);
    CHECK(f == nullptr);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer: 1-track silent fixture mixes to silent output") {
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) {
        MESSAGE("skipping: audio fixture not available");
        return;
    }

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};

    me::audio::AudioTrackFeed feed;
    std::string err;
    REQUIRE(me::audio::open_audio_track_feed(
                demux, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(std::move(feed),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
    CHECK(mixer.track_count() == 1);

    int frames = 0;
    while (true) {
        AVFrame* mixed = nullptr;
        me_status_t s = mixer.pull_next_mixed_frame(&mixed, &err);
        if (s == ME_E_NOT_FOUND) break;
        REQUIRE(s == ME_OK);
        REQUIRE(mixed != nullptr);
        CHECK(mixed->sample_rate == 48000);
        CHECK(mixed->format == AV_SAMPLE_FMT_FLTP);
        CHECK(mixed->ch_layout.nb_channels == 1);
        CHECK(mixed->nb_samples == 1024);
        /* Silence in → silence out (regardless of peak_limiter). */
        auto* p = reinterpret_cast<const float*>(mixed->extended_data[0]);
        for (int i = 0; i < mixed->nb_samples; ++i) {
            CHECK(p[i] == 0.0f);
        }
        av_frame_free(&mixed);
        ++frames;
        if (frames > 200) break;  /* safety */
    }
    CHECK(frames >= 1);
    CHECK(mixer.eof());
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer: 2-track silent fixtures mix to silent output") {
    /* Two independent feeds reading the same silent fixture. Sum
     * of two silences is silence; the mixer should drive both
     * FIFOs in lockstep until both drain. */
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    auto demux1 = open_demux(fixture);
    auto demux2 = open_demux(fixture);
    REQUIRE(demux1);
    REQUIRE(demux2);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};

    me::audio::AudioTrackFeed f1, f2;
    std::string err;
    REQUIRE(me::audio::open_audio_track_feed(
                demux1, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, f1, &err)
            == ME_OK);
    REQUIRE(me::audio::open_audio_track_feed(
                demux2, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, f2, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(std::move(f1),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
    REQUIRE(mixer.add_track(std::move(f2),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
    CHECK(mixer.track_count() == 2);

    int frames = 0;
    while (true) {
        AVFrame* mixed = nullptr;
        me_status_t s = mixer.pull_next_mixed_frame(&mixed, &err);
        if (s == ME_E_NOT_FOUND) break;
        REQUIRE(s == ME_OK);
        REQUIRE(mixed != nullptr);
        CHECK(mixed->nb_samples == 1024);
        auto* p = reinterpret_cast<const float*>(mixed->extended_data[0]);
        for (int i = 0; i < mixed->nb_samples; ++i) {
            CHECK(p[i] == 0.0f);
        }
        av_frame_free(&mixed);
        ++frames;
        if (frames > 200) break;
    }
    CHECK(frames >= 1);
    CHECK(mixer.eof());
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("build_audio_mixer_for_timeline: no audio clips → ME_E_NOT_FOUND") {
    me::Timeline tl;
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    me::Clip vc;
    vc.id = "c0"; vc.track_id = "v0"; vc.type = me::ClipType::Video;
    vc.asset_id = "a1";
    tl.clips.push_back(std::move(vc));

    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);

    std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes(1);
    std::unique_ptr<me::audio::AudioMixer> mixer;
    std::string err;
    CHECK(me::audio::build_audio_mixer_for_timeline(tl, pool, demuxes, cfg, mixer, &err)
          == ME_E_NOT_FOUND);
    CHECK(err.find("no audio clips") != std::string::npos);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("build_audio_mixer_for_timeline: size mismatch between clips and demux list → ME_E_INVALID_ARG") {
    me::Timeline tl;
    tl.tracks.push_back(me::Track{"a0", me::TrackKind::Audio, true});
    me::Clip ac;
    ac.id = "c0"; ac.track_id = "a0"; ac.type = me::ClipType::Audio;
    ac.asset_id = "a1";
    tl.clips.push_back(std::move(ac));

    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);

    std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes;   /* empty, mismatch */
    std::unique_ptr<me::audio::AudioMixer> mixer;
    std::string err;
    CHECK(me::audio::build_audio_mixer_for_timeline(tl, pool, demuxes, cfg, mixer, &err)
          == ME_E_INVALID_ARG);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("build_audio_mixer_for_timeline: 1 audio clip + fixture demux builds a pullable mixer") {
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    me::Timeline tl;
    tl.tracks.push_back(me::Track{"a0", me::TrackKind::Audio, true});
    me::Clip ac;
    ac.id = "c0"; ac.track_id = "a0"; ac.type = me::ClipType::Audio;
    ac.asset_id = "a1";
    ac.gain_db = me::AnimatedNumber::from_static(-6.0);   /* ~0.501 linear — still silent input * anything = 0 */
    ac.time_start   = me_rational_t{0, 48000};
    ac.time_duration = me_rational_t{48000, 48000};
    ac.source_start = me_rational_t{0, 48000};
    tl.clips.push_back(std::move(ac));

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes{demux};

    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);

    std::unique_ptr<me::audio::AudioMixer> mixer;
    std::string err;
    REQUIRE(me::audio::build_audio_mixer_for_timeline(tl, pool, demuxes, cfg, mixer, &err)
            == ME_OK);
    REQUIRE(mixer);
    CHECK(mixer->track_count() == 1);

    /* Pull at least one frame. Silent input × any gain = silent output. */
    AVFrame* f = nullptr;
    REQUIRE(mixer->pull_next_mixed_frame(&f, &err) == ME_OK);
    REQUIRE(f != nullptr);
    CHECK(f->nb_samples == 1024);
    CHECK(f->sample_rate == 48000);
    auto* p = reinterpret_cast<const float*>(f->extended_data[0]);
    for (int i = 0; i < f->nb_samples; ++i) CHECK(p[i] == 0.0f);
    av_frame_free(&f);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("build_audio_mixer_for_timeline: null demux for audio clip → ME_E_INVALID_ARG") {
    me::Timeline tl;
    tl.tracks.push_back(me::Track{"a0", me::TrackKind::Audio, true});
    me::Clip ac;
    ac.id = "c0"; ac.track_id = "a0"; ac.type = me::ClipType::Audio;
    ac.asset_id = "a1";
    tl.clips.push_back(std::move(ac));

    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);

    std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes(1);   /* null entry */
    std::unique_ptr<me::audio::AudioMixer> mixer;
    std::string err;
    CHECK(me::audio::build_audio_mixer_for_timeline(tl, pool, demuxes, cfg, mixer, &err)
          == ME_E_INVALID_ARG);
    CHECK(err.find("null demux") != std::string::npos);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

namespace {

/* Construct an AudioTrackFeed suitable for inject_samples_for_test:
 * target params match the mixer's cfg (so add_track passes
 * validation), eof=true so pull_next_mixed_frame won't try to pull
 * from the feed. */
me::audio::AudioTrackFeed make_test_only_feed(const me::audio::AudioMixerConfig& cfg) {
    me::audio::AudioTrackFeed feed;
    feed.target_rate = cfg.target_rate;
    feed.target_fmt  = cfg.target_fmt;
    av_channel_layout_copy(&feed.target_ch_layout, &cfg.target_ch_layout);
    feed.eof = true;
    return feed;
}

}  // namespace

TEST_CASE("AudioMixer: 2-track injection below peak threshold — sum is exact passthrough") {
    /* 2 tracks × mono × 1024 samples of constant 0.25 each.
     * Expected mixed output: 0.5 per sample (below 0.95 threshold →
     * peak_limiter is pass-through → no compression). This pins the
     * mix_samples path through AudioMixer using non-trivial (but
     * below-limit) values. */
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    std::string err;
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());

    REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
    REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);

    std::vector<float> plane0(cfg.frame_size, 0.25f);
    std::vector<float> plane1(cfg.frame_size, 0.25f);
    const float* planes_t0[1] = {plane0.data()};
    const float* planes_t1[1] = {plane1.data()};
    REQUIRE(mixer.inject_samples_for_test(0, planes_t0, cfg.frame_size, &err) == ME_OK);
    REQUIRE(mixer.inject_samples_for_test(1, planes_t1, cfg.frame_size, &err) == ME_OK);

    AVFrame* out = nullptr;
    REQUIRE(mixer.pull_next_mixed_frame(&out, &err) == ME_OK);
    REQUIRE(out != nullptr);
    auto* mixed = reinterpret_cast<const float*>(out->extended_data[0]);
    for (int i = 0; i < out->nb_samples; ++i) {
        CHECK(mixed[i] == doctest::Approx(0.5f).epsilon(1e-6f));
    }
    av_frame_free(&out);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer: 2-track injection above peak threshold triggers soft-knee limiter") {
    /* 2 tracks × mono × 1024 samples of constant 0.8 each.
     * Raw sum = 1.6 (well above 0.95 threshold). peak_limiter
     * compresses: out = sign(x) * (0.95 + 0.05 * tanh((|x| - 0.95) /
     * 0.05)). For x=1.6: tanh((1.6 - 0.95) / 0.05) = tanh(13) ≈ 1.0,
     * so out ≈ 0.95 + 0.05 * 1.0 = 1.0. Asserts |output| ≤ 1.0 and
     * strictly less than raw sum 1.6 — pinning that the limiter
     * actually runs and the mixer passes non-zero sums through it. */
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    std::string err;
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
    REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);

    std::vector<float> plane0(cfg.frame_size, 0.8f);
    std::vector<float> plane1(cfg.frame_size, 0.8f);
    const float* planes_t0[1] = {plane0.data()};
    const float* planes_t1[1] = {plane1.data()};
    REQUIRE(mixer.inject_samples_for_test(0, planes_t0, cfg.frame_size, &err) == ME_OK);
    REQUIRE(mixer.inject_samples_for_test(1, planes_t1, cfg.frame_size, &err) == ME_OK);

    AVFrame* out = nullptr;
    REQUIRE(mixer.pull_next_mixed_frame(&out, &err) == ME_OK);
    REQUIRE(out != nullptr);
    auto* mixed = reinterpret_cast<const float*>(out->extended_data[0]);
    for (int i = 0; i < out->nb_samples; ++i) {
        CHECK(std::abs(mixed[i]) <= 1.0f);       /* limiter caps at ±1 */
        CHECK(mixed[i] < 1.6f);                  /* below raw sum */
        CHECK(mixed[i] > 0.95f);                 /* past threshold → compressed non-trivially */
    }
    av_frame_free(&out);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer: synthetic sine-wave mix is bit-identical across two runs (determinism)") {
    /* Cross-track sine mix through AudioMixer's full path (FIFO
     * pull + mix_samples + peak_limiter). Two mixers built with
     * identical config, fed identical synthetic waveforms. Output
     * bytes must match exactly — VISION §5.3 determinism on the
     * software path. */
    auto run_once = [](std::vector<float>& out_samples) {
        LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
        me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
        std::string err;
        me::audio::AudioMixer mixer(cfg, &err);
        REQUIRE(mixer.ok());
        REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);
        REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);

        /* 50 Hz sine (track 0) + 100 Hz sine (track 1) at 48kHz,
         * both at 0.4 amplitude. Raw sum peaks at 0.8 → below
         * limiter threshold. */
        std::vector<float> t0(cfg.frame_size), t1(cfg.frame_size);
        constexpr double PI = 3.14159265358979323846;
        for (int i = 0; i < cfg.frame_size; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            t0[i] = static_cast<float>(0.4 * std::sin(2.0 * PI * 50.0  * t));
            t1[i] = static_cast<float>(0.4 * std::sin(2.0 * PI * 100.0 * t));
        }
        const float* planes_t0[1] = {t0.data()};
        const float* planes_t1[1] = {t1.data()};
        REQUIRE(mixer.inject_samples_for_test(0, planes_t0, cfg.frame_size, &err) == ME_OK);
        REQUIRE(mixer.inject_samples_for_test(1, planes_t1, cfg.frame_size, &err) == ME_OK);

        AVFrame* out = nullptr;
        REQUIRE(mixer.pull_next_mixed_frame(&out, &err) == ME_OK);
        REQUIRE(out != nullptr);
        auto* mixed = reinterpret_cast<const float*>(out->extended_data[0]);
        out_samples.assign(mixed, mixed + out->nb_samples);
        av_frame_free(&out);
        av_channel_layout_uninit(&cfg.target_ch_layout);
    };

    std::vector<float> run_a, run_b;
    run_once(run_a);
    run_once(run_b);
    REQUIRE(run_a.size() == run_b.size());
    REQUIRE(run_a.size() == 1024u);
    for (std::size_t i = 0; i < run_a.size(); ++i) {
        CHECK(run_a[i] == run_b[i]);   /* bit-identical */
    }
    /* Non-silent sanity: at least one non-zero sample. */
    bool has_non_zero = false;
    for (float s : run_a) if (s != 0.0f) { has_non_zero = true; break; }
    CHECK(has_non_zero);
}

TEST_CASE("AudioMixer::inject_samples_for_test: invalid args rejected") {
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    std::string err;
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(make_test_only_feed(cfg),
                             me::AnimatedNumber::from_static(0.0), &err) == ME_OK);

    std::vector<float> plane(128, 0.0f);
    const float* planes[1] = {plane.data()};
    /* Bad track index */
    CHECK(mixer.inject_samples_for_test(99, planes, 128, &err) == ME_E_INVALID_ARG);
    /* Null plane_data */
    CHECK(mixer.inject_samples_for_test(0, nullptr, 128, &err) == ME_E_INVALID_ARG);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer::add_track: feed with mismatched target rejected") {
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};

    /* Open feed at 44100 Hz — mismatch with mixer's 48000. */
    me::audio::AudioTrackFeed feed;
    std::string err;
    REQUIRE(me::audio::open_audio_track_feed(
                demux, pool, 44100, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);   /* 48000 */
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    CHECK(mixer.add_track(std::move(feed),
                           me::AnimatedNumber::from_static(0.0), &err) == ME_E_INVALID_ARG);
    CHECK(mixer.track_count() == 0);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}

TEST_CASE("AudioMixer: animated gain_db interpolates linearly across emitted frames") {
    /* Pin the animated-gain path: inject a constant 0.4-amplitude
     * signal over 2 frames (2048 samples at 48 kHz, so T spans
     * [0, 2048/48000)). gain_db keyframes ramp linearly from 0 dB
     * at T=0 to -20 dB at T=2048/48000. Each emitted frame's T is
     * the frame-start cursor:
     *   - Frame 0: T={0, 48000} → gain_db = 0 dB → linear = 1.0
     *              → expected samples ≈ 0.4
     *   - Frame 1: T={1024, 48000} (linear midpoint) →
     *              gain_db = -10 dB → linear ≈ 0.316 → samples ≈ 0.126
     * Assert monotone decrease and approximate amplitudes. This
     * would have been impossible before the gain_db migration
     * (feed's gain_linear was ctor-frozen). */
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    std::string err;
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());

    me::AnimatedNumber ramp = me::AnimatedNumber::from_keyframes({
        {me_rational_t{0,    48000}, 0.0,   me::Interp::Linear, {0,0,1,1}},
        {me_rational_t{2048, 48000}, -20.0, me::Interp::Linear, {0,0,1,1}},
    });
    REQUIRE(mixer.add_track(make_test_only_feed(cfg), std::move(ramp), &err) == ME_OK);

    /* Inject 2 frames worth of constant 0.4 into the FIFO — the
     * mixer is eof (test feed has eof=true), so it consumes the
     * injected samples and stops when the FIFO drains. */
    std::vector<float> plane(static_cast<std::size_t>(cfg.frame_size) * 2, 0.4f);
    const float* planes[1] = {plane.data()};
    REQUIRE(mixer.inject_samples_for_test(
                0, planes,
                static_cast<std::size_t>(cfg.frame_size) * 2, &err) == ME_OK);

    /* Frame 0 — T={0, 48000}, gain_db = 0, linear = 1.0. */
    AVFrame* f0 = nullptr;
    REQUIRE(mixer.pull_next_mixed_frame(&f0, &err) == ME_OK);
    REQUIRE(f0 != nullptr);
    auto* s0 = reinterpret_cast<const float*>(f0->extended_data[0]);
    const float first_sample_0 = s0[0];
    const float last_sample_0  = s0[cfg.frame_size - 1];
    CHECK(first_sample_0 == doctest::Approx(0.4f).epsilon(1e-4f));
    CHECK(last_sample_0  == doctest::Approx(0.4f).epsilon(1e-4f));
    av_frame_free(&f0);

    /* Frame 1 — T={1024, 48000}, gain_db midpoint = -10 dB,
     * linear ≈ 0.31623 → samples ≈ 0.1265. */
    AVFrame* f1 = nullptr;
    REQUIRE(mixer.pull_next_mixed_frame(&f1, &err) == ME_OK);
    REQUIRE(f1 != nullptr);
    auto* s1 = reinterpret_cast<const float*>(f1->extended_data[0]);
    const float first_sample_1 = s1[0];
    CHECK(first_sample_1 == doctest::Approx(0.4f * 0.31623f).epsilon(1e-3f));
    /* All samples in frame 1 held at the frame-start T (per-frame-
     * constant gain is the documented buffer-level approximation). */
    for (int i = 0; i < f1->nb_samples; ++i) {
        CHECK(s1[i] == doctest::Approx(first_sample_1).epsilon(1e-6f));
    }
    /* Strict monotone decrease — the point of animated gain. */
    CHECK(first_sample_1 < first_sample_0);
    av_frame_free(&f1);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}
