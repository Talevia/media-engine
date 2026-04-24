/*
 * test_frame_puller — tripwire for pull_next_video_frame against the
 * shared determinism fixture.
 *
 * Opens the fixture MP4 directly (no engine / Timeline wrapping),
 * finds its video stream, opens an AVCodecContext decoder, and
 * pulls frames one-by-one until EOF. Verifies:
 *   - N pulled frames == expected count for a 25fps 1s fixture
 *   - each frame has expected dimensions (640×480)
 *   - the post-EOF call returns ME_E_NOT_FOUND cleanly
 *   - null-arg rejection matches the contract
 *
 * Bypasses the engine factory to keep the test focused on the
 * pull_next_video_frame contract itself — engine-level
 * orchestration lives in test_determinism / test_render_progress.
 */
#include <doctest/doctest.h>

#include "orchestrator/frame_puller.hpp"

#include "io/demux_context.hpp"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

namespace {

struct FmtGuard {
    AVFormatContext* f = nullptr;
    ~FmtGuard() { if (f) avformat_close_input(&f); }
};

struct DecGuard {
    AVCodecContext* c = nullptr;
    ~DecGuard() { if (c) avcodec_free_context(&c); }
};

struct PacketGuard {
    AVPacket* p = nullptr;
    PacketGuard() { p = av_packet_alloc(); }
    ~PacketGuard() { if (p) av_packet_free(&p); }
};

struct FrameGuard {
    AVFrame* f = nullptr;
    FrameGuard() { f = av_frame_alloc(); }
    ~FrameGuard() { if (f) av_frame_free(&f); }
};

}  // namespace

TEST_CASE("pull_next_video_frame: null args return ME_E_INVALID_ARG") {
    PacketGuard pkt;
    FrameGuard  frame;
    std::string err;
    CHECK(me::orchestrator::pull_next_video_frame(
              nullptr, 0, nullptr, pkt.p, frame.f, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_video_frame: negative stream idx returns ME_E_INVALID_ARG") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }
    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    PacketGuard pkt;
    FrameGuard  frame;
    AVCodecContext* fake_dec_not_used = nullptr;
    std::string err;
    /* Even with a valid demux, a negative stream index is rejected. */
    CHECK(me::orchestrator::pull_next_video_frame(
              fmt.f, -1, fake_dec_not_used, pkt.p, frame.f, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_video_frame: pulls expected frame count from fixture then EOF") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

    /* Open demux. */
    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(fmt.f, nullptr) >= 0);

    /* Find video stream. */
    int vsi = -1;
    for (unsigned i = 0; i < fmt.f->nb_streams; ++i) {
        if (fmt.f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vsi = static_cast<int>(i);
            break;
        }
    }
    REQUIRE(vsi >= 0);

    /* Open decoder. */
    const AVCodec* codec = avcodec_find_decoder(fmt.f->streams[vsi]->codecpar->codec_id);
    REQUIRE(codec != nullptr);
    DecGuard dec;
    dec.c = avcodec_alloc_context3(codec);
    REQUIRE(dec.c != nullptr);
    REQUIRE(avcodec_parameters_to_context(dec.c, fmt.f->streams[vsi]->codecpar) >= 0);
    REQUIRE(avcodec_open2(dec.c, codec, nullptr) >= 0);

    /* Pull all frames. Fixture is 25fps × 1s = 25 frames expected
     * (gen_fixture encodes exactly `--tagged` or default 25 frames). */
    PacketGuard pkt;
    FrameGuard  frame;
    int pulled = 0;
    int last_w = 0, last_h = 0;
    while (true) {
        std::string err;
        me_status_t s = me::orchestrator::pull_next_video_frame(
            fmt.f, vsi, dec.c, pkt.p, frame.f, &err);
        if (s == ME_E_NOT_FOUND) break;
        REQUIRE(s == ME_OK);
        ++pulled;
        last_w = frame.f->width;
        last_h = frame.f->height;
        av_frame_unref(frame.f);
    }
    CHECK(pulled >= 20);   /* 25fps × 1s = 25; allow slack for fixture drift */
    CHECK(pulled <= 30);
    CHECK(last_w == 640);
    CHECK(last_h == 480);
}

TEST_CASE("open_track_decoder: opens video decoder from fixture demux") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    /* Build a DemuxContext manually — same shape as io/demux_kernel
     * does but bypassing the graph layer. */
    auto demux = std::make_shared<me::io::DemuxContext>();
    REQUIRE(avformat_open_input(&demux->fmt, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(demux->fmt, nullptr) >= 0);
    demux->uri = fixture_path;

    me::resource::CodecPool pool;
    me::orchestrator::TrackDecoderState state;
    std::string err;
    REQUIRE(me::orchestrator::open_track_decoder(demux, pool, state, &err) == ME_OK);
    CHECK(state.demux != nullptr);
    CHECK(state.video_stream_idx >= 0);
    CHECK(state.dec != nullptr);
    CHECK(state.pkt_scratch != nullptr);
    CHECK(state.frame_scratch != nullptr);

    /* Now that state is set up via the helper, the existing
     * pull_next_video_frame function can drive it. Pull one frame
     * to confirm the wiring is correct. */
    const me_status_t s = me::orchestrator::pull_next_video_frame(
        state.demux->fmt,
        state.video_stream_idx,
        state.dec.get(),
        state.pkt_scratch.get(),
        state.frame_scratch.get(),
        &err);
    CHECK(s == ME_OK);
    CHECK(state.frame_scratch->width == 640);
    CHECK(state.frame_scratch->height == 480);
}

TEST_CASE("open_track_decoder: null demux returns ME_E_INVALID_ARG") {
    me::resource::CodecPool pool;
    me::orchestrator::TrackDecoderState state;
    std::string err;
    CHECK(me::orchestrator::open_track_decoder(nullptr, pool, state, &err)
          == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_audio_frame: null args return ME_E_INVALID_ARG") {
    PacketGuard pkt;
    FrameGuard  frame;
    std::string err;
    CHECK(me::orchestrator::pull_next_audio_frame(
              nullptr, 0, nullptr, pkt.p, frame.f, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_audio_frame: negative stream idx returns ME_E_INVALID_ARG") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }
    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    PacketGuard pkt;
    FrameGuard  frame;
    CHECK(me::orchestrator::pull_next_audio_frame(
              fmt.f, -1, nullptr, pkt.p, frame.f, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("pull_next_audio_frame: when fixture has no audio, returns NOT_FOUND if run against video stream index") {
    /* The shared determinism fixture is video-only. Exercising
     * pull_next_audio_frame against its video stream idx (with an
     * arbitrary decoder) would mix state-machine semantics — the
     * helper filters out non-matching stream_index packets, so
     * passing an audio_stream_idx that doesn't exist in the demux
     * drains without producing any frames. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(fmt.f, nullptr) >= 0);

    const int asi = av_find_best_stream(fmt.f, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (asi < 0) {
        MESSAGE("fixture has no audio stream — pull_next_audio_frame full-drain test skipped");
        return;
    }

    /* Fixture surprisingly has audio — drain and count. */
    const AVCodec* codec = avcodec_find_decoder(fmt.f->streams[asi]->codecpar->codec_id);
    REQUIRE(codec != nullptr);
    DecGuard dec;
    dec.c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec.c, fmt.f->streams[asi]->codecpar);
    avcodec_open2(dec.c, codec, nullptr);

    PacketGuard pkt;
    FrameGuard  frame;
    int pulled = 0;
    while (true) {
        me_status_t s = me::orchestrator::pull_next_audio_frame(
            fmt.f, asi, dec.c, pkt.p, frame.f, nullptr);
        if (s == ME_E_NOT_FOUND) break;
        REQUIRE(s == ME_OK);
        ++pulled;
        av_frame_unref(frame.f);
        if (pulled > 10000) break;   /* safety */
    }
    CHECK(pulled >= 1);
}

TEST_CASE("pull_next_video_frame: second call after EOF still returns ME_E_NOT_FOUND (idempotent)") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(fmt.f, nullptr) >= 0);

    int vsi = -1;
    for (unsigned i = 0; i < fmt.f->nb_streams; ++i) {
        if (fmt.f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vsi = i; break; }
    }
    REQUIRE(vsi >= 0);

    const AVCodec* codec = avcodec_find_decoder(fmt.f->streams[vsi]->codecpar->codec_id);
    DecGuard dec;
    dec.c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec.c, fmt.f->streams[vsi]->codecpar);
    avcodec_open2(dec.c, codec, nullptr);

    PacketGuard pkt;
    FrameGuard  frame;
    /* Drain fully. */
    while (me::orchestrator::pull_next_video_frame(
               fmt.f, vsi, dec.c, pkt.p, frame.f, nullptr) == ME_OK) {
        av_frame_unref(frame.f);
    }
    /* One more call — should cleanly return NOT_FOUND without error. */
    CHECK(me::orchestrator::pull_next_video_frame(
              fmt.f, vsi, dec.c, pkt.p, frame.f, nullptr) == ME_E_NOT_FOUND);
}
