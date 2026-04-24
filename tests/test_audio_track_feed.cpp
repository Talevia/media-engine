/*
 * test_audio_track_feed — per-track audio feed contract.
 *
 * Exercises open_audio_track_feed + pull_next_processed_audio_frame
 * against the shared --with-audio determinism fixture (48 frames ×
 * 1024 samples silent AAC, mono 48 kHz). The feed is the building
 * block for the upcoming AudioMixer — one feed per audio track.
 */
#include <doctest/doctest.h>

#include "audio/track_feed.hpp"
#include "io/demux_context.hpp"
#include "resource/codec_pool.hpp"

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

}  // namespace

TEST_CASE("open_audio_track_feed: null demux returns ME_E_INVALID_ARG") {
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioTrackFeed feed;
    std::string err;
    CHECK(me::audio::open_audio_track_feed(
              nullptr, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
          == ME_E_INVALID_ARG);
}

TEST_CASE("open_audio_track_feed: invalid target_rate returns ME_E_INVALID_ARG") {
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioTrackFeed feed;
    std::string err;
    CHECK(me::audio::open_audio_track_feed(
              demux, pool, 0, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
          == ME_E_INVALID_ARG);
}

TEST_CASE("open_audio_track_feed: demux with no audio returns ME_E_NOT_FOUND") {
    /* The video-only determinism fixture has no audio stream. */
#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif
    const std::string fixture = ME_TEST_FIXTURE_MP4;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioTrackFeed feed;
    std::string err;
    CHECK(me::audio::open_audio_track_feed(
              demux, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
          == ME_E_NOT_FOUND);
}

TEST_CASE("open_audio_track_feed: configures fields on success") {
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
    CHECK(feed.audio_stream_idx >= 0);
    CHECK(feed.target_rate == 48000);
    CHECK(feed.target_fmt == AV_SAMPLE_FMT_FLTP);
    CHECK(feed.target_ch_layout.nb_channels == 1);
    CHECK_FALSE(feed.eof);
}

TEST_CASE("pull_next_processed_audio_frame: drains feed to EOF with target-format frames") {
    const std::string fixture = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture.empty() || !fs::exists(fixture)) { return; }

    auto demux = open_demux(fixture);
    REQUIRE(demux);
    me::resource::CodecPool pool;
    LayoutGuard mono{AV_CHANNEL_LAYOUT_MONO};
    me::audio::AudioTrackFeed feed;
    std::string err;
    REQUIRE(me::audio::open_audio_track_feed(
                demux, pool, 48000, AV_SAMPLE_FMT_FLTP, mono.l, feed, &err)
            == ME_OK);

    int pulled = 0;
    while (true) {
        AVFrame* f = nullptr;
        me_status_t s = me::audio::pull_next_processed_audio_frame(feed, &f, &err);
        if (s == ME_E_NOT_FOUND) break;
        REQUIRE(s == ME_OK);
        REQUIRE(f != nullptr);
        CHECK(f->sample_rate == 48000);
        CHECK(f->format == AV_SAMPLE_FMT_FLTP);
        CHECK(f->ch_layout.nb_channels == 1);
        av_frame_free(&f);
        ++pulled;
        if (pulled > 200) break;   /* safety */
    }
    CHECK(pulled >= 1);
    CHECK(feed.eof);
}

TEST_CASE("pull_next_processed_audio_frame: null out_frame returns ME_E_INVALID_ARG") {
    me::audio::AudioTrackFeed feed;
    std::string err;
    CHECK(me::audio::pull_next_processed_audio_frame(feed, nullptr, &err)
          == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_processed_audio_frame: unopened feed returns ME_E_INVALID_ARG") {
    me::audio::AudioTrackFeed feed;   /* default-constructed; never opened */
    AVFrame* f = nullptr;
    std::string err;
    CHECK(me::audio::pull_next_processed_audio_frame(feed, &f, &err)
          == ME_E_INVALID_ARG);
    CHECK(f == nullptr);
}
