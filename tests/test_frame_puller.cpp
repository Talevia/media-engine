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
#include "fixture_skip.hpp"

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
    ME_REQUIRE_FIXTURE(fixture_path);
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
    ME_REQUIRE_FIXTURE(fixture_path);
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

TEST_CASE("pull_next_audio_frame: drains silent AAC audio from --with-audio fixture") {
    /* Uses the audio-capable variant of the determinism fixture
     * (gen_fixture --with-audio) so this test exercises the real
     * AAC decode path rather than falling through to skip. The
     * fixture is declared via ME_TEST_FIXTURE_MP4_WITH_AUDIO by
     * tests/CMakeLists.txt, which wires the dependency. */
#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO
#define ME_TEST_FIXTURE_MP4_WITH_AUDIO ""
#endif
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    ME_REQUIRE_FIXTURE(fixture_path);
    FmtGuard fmt;
    REQUIRE(avformat_open_input(&fmt.f, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(fmt.f, nullptr) >= 0);

    const int asi = av_find_best_stream(fmt.f, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    REQUIRE(asi >= 0);  /* --with-audio variant must have audio */

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

TEST_CASE("seek_track_decoder_frame_accurate_to: lands on target frame after decoder advanced past it") {
    /* Simulates the cross-dissolve realignment scenario: open a
     * decoder, advance it past a target source_time, then call the
     * frame-accurate seek and verify the loaded frame's pts matches
     * the target (rescaled to stream time base). Fixture is 25 fps,
     * stream time_base = {1, 25}, so source_time = {10, 25} s maps
     * to pts 10. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    auto demux = std::make_shared<me::io::DemuxContext>();
    REQUIRE(avformat_open_input(&demux->fmt, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(demux->fmt, nullptr) >= 0);
    demux->uri = fixture_path;

    me::resource::CodecPool pool;
    me::orchestrator::TrackDecoderState td;
    std::string err;
    REQUIRE(me::orchestrator::open_track_decoder(demux, pool, td, &err) == ME_OK);

    /* Drive decoder forward past target (pull 15 frames — target is
     * frame 10). */
    for (int i = 0; i < 15; ++i) {
        me_status_t s = me::orchestrator::pull_next_video_frame(
            td.demux->fmt, td.video_stream_idx, td.dec.get(),
            td.pkt_scratch.get(), td.frame_scratch.get(), &err);
        REQUIRE(s == ME_OK);
        av_frame_unref(td.frame_scratch.get());
    }

    /* Seek back to source_time = {10, 25} s (400 ms). */
    const me_rational_t target{10, 25};
    const me_status_t ss = me::orchestrator::seek_track_decoder_frame_accurate_to(
        td, target, &err);
    REQUIRE(ss == ME_OK);
    REQUIRE(td.frame_scratch->width == 640);
    REQUIRE(td.frame_scratch->height == 480);

    AVStream* st = td.demux->fmt->streams[td.video_stream_idx];
    const int64_t expected_pts_stb = av_rescale_q(
        400000,  /* target_us = 10 * 1_000_000 / 25 */
        AV_TIME_BASE_Q, st->time_base);
    const int64_t got_pts = (td.frame_scratch->pts != AV_NOPTS_VALUE)
        ? td.frame_scratch->pts
        : td.frame_scratch->best_effort_timestamp;
    /* Frame-accurate: the returned frame's pts is at-or-after target,
     * and since MPEG-4 Part 2 without B-frames is strictly sequential
     * per-frame pts, "at" is the exact match for integer time-base
     * targets. Allow ±1 pts slack for demuxer quirks but pin to the
     * narrow window — proves the seek didn't overshoot / undershoot. */
    CHECK(got_pts >= expected_pts_stb);
    CHECK(got_pts <= expected_pts_stb + 1);
    av_frame_unref(td.frame_scratch.get());
}

TEST_CASE("seek_track_decoder_frame_accurate_to: unopened td returns ME_E_INVALID_ARG") {
    me::orchestrator::TrackDecoderState td;  /* default: no demux, no dec */
    std::string err;
    CHECK(me::orchestrator::seek_track_decoder_frame_accurate_to(
              td, me_rational_t{1, 1}, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("seek_track_decoder_frame_accurate_to: negative target clamps to zero") {
    /* Negative source_time is nonsensical but callers may construct
     * it via rational arithmetic with undersized numerators. The
     * helper clamps to 0 and returns the first frame. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    auto demux = std::make_shared<me::io::DemuxContext>();
    REQUIRE(avformat_open_input(&demux->fmt, fixture_path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(demux->fmt, nullptr) >= 0);
    demux->uri = fixture_path;

    me::resource::CodecPool pool;
    me::orchestrator::TrackDecoderState td;
    std::string err;
    REQUIRE(me::orchestrator::open_track_decoder(demux, pool, td, &err) == ME_OK);

    const me_rational_t target{-5, 1};
    REQUIRE(me::orchestrator::seek_track_decoder_frame_accurate_to(
                td, target, &err) == ME_OK);
    /* First decodable frame. MPEG-4 Part 2 with GOP=25 + max_b=0,
     * the fixture's pts 0 is the first keyframe. */
    const int64_t got_pts = (td.frame_scratch->pts != AV_NOPTS_VALUE)
        ? td.frame_scratch->pts
        : td.frame_scratch->best_effort_timestamp;
    CHECK(got_pts == 0);
    av_frame_unref(td.frame_scratch.get());
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
