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

#include <filesystem>
#include <memory>
#include <string>

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
                demux, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, 1.0f, feed, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(std::move(feed), &err) == ME_OK);
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
                demux1, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, 0.5f, f1, &err)
            == ME_OK);
    REQUIRE(me::audio::open_audio_track_feed(
                demux2, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, 0.5f, f2, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    REQUIRE(mixer.add_track(std::move(f1), &err) == ME_OK);
    REQUIRE(mixer.add_track(std::move(f2), &err) == ME_OK);
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
    ac.gain_db = -6.0;   /* ~0.501 linear — still silent input * anything = 0 */
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
                demux, pool, 44100, AV_SAMPLE_FMT_FLTP, mono.l, 1.0f, feed, &err)
            == ME_OK);

    me::audio::AudioMixerConfig cfg = make_cfg(mono.l);   /* 48000 */
    me::audio::AudioMixer mixer(cfg, &err);
    REQUIRE(mixer.ok());
    CHECK(mixer.add_track(std::move(feed), &err) == ME_E_INVALID_ARG);
    CHECK(mixer.track_count() == 0);
    av_channel_layout_uninit(&cfg.target_ch_layout);
}
