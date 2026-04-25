#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "fixture_skip.hpp"

namespace fs = std::filesystem;

namespace {

/* Slurp a file into a byte vector. Empty vector on error. */
std::vector<unsigned char> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),
                                       std::istreambuf_iterator<char>());
}

/* Render the given timeline JSON with a configurable output spec. Fresh
 * engine per call — so determinism holds across engine instantiations, not
 * just back-to-back renders within a single process. */
me_status_t render_with_spec(const std::string& timeline_json,
                              const std::string& out_path,
                              const char*        video_codec,
                              const char*        audio_codec) {
    me_engine_t* eng = nullptr;
    me_status_t s = me_engine_create(nullptr, &eng);
    if (s != ME_OK) return s;

    me_timeline_t* tl = nullptr;
    s = me_timeline_load_json(eng, timeline_json.data(), timeline_json.size(), &tl);
    if (s != ME_OK) { me_engine_destroy(eng); return s; }

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = video_codec;
    spec.audio_codec = audio_codec;

    me_render_job_t* job = nullptr;
    s = me_render_start(eng, tl, &spec, nullptr, nullptr, &job);
    if (s != ME_OK) {
        me_timeline_destroy(tl);
        me_engine_destroy(eng);
        return s;
    }
    s = me_render_wait(job);
    me_render_job_destroy(job);
    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return s;
}

me_status_t render_passthrough(const std::string& timeline_json,
                                const std::string& out_path) {
    return render_with_spec(timeline_json, out_path, "passthrough", "passthrough");
}

}  // namespace

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

TEST_CASE("passthrough is byte-deterministic across two independent renders") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    /* Timeline with a single clip referencing the fixture.
     * Fixture is 25 frames at 25 fps = 1 second; timeRange matches. */
    namespace tb = me::tests::tb;
    const std::string timeline_json = tb::TimelineBuilder()
        .frame_rate(25, 1).resolution(640, 480)
        .add_asset(tb::AssetSpec{.uri = "file://" + fixture_path})
        .add_clip(tb::ClipSpec{
            .time_start_den = 25, .time_dur_num = 25, .time_dur_den = 25,
            .source_start_den = 25, .source_dur_num = 25, .source_dur_den = 25,
        })
        .build();

    const fs::path tmp_dir = fs::temp_directory_path() / "me-determinism-test";
    fs::create_directories(tmp_dir);
    const fs::path out1 = tmp_dir / "out1.mp4";
    const fs::path out2 = tmp_dir / "out2.mp4";
    fs::remove(out1);
    fs::remove(out2);

    REQUIRE(render_passthrough(timeline_json, out1.string()) == ME_OK);
    REQUIRE(render_passthrough(timeline_json, out2.string()) == ME_OK);

    const auto bytes1 = slurp(out1);
    const auto bytes2 = slurp(out2);

    REQUIRE(!bytes1.empty());
    REQUIRE(!bytes2.empty());
    CHECK(bytes1.size() == bytes2.size());

    /* Byte-for-byte identical. VISION §3.3 / §5.3: same inputs → same bytes
     * on the software path. MP4 stream-copy is software; no HW encoder or
     * time-variant metadata in play, so this is trivially required to hold
     * — the test exists as a tripwire for future regressions (e.g. someone
     * adds wall-clock tagging or non-deterministic packet interleaving). */
    if (bytes1 != bytes2) {
        /* Locate first divergence for diagnosis. */
        size_t i = 0;
        while (i < bytes1.size() && i < bytes2.size() && bytes1[i] == bytes2[i]) ++i;
        FAIL("outputs differ at byte offset " << i);
    } else {
        CHECK(true);  /* byte-identical */
    }
}

TEST_CASE("passthrough determinism holds across engine restarts") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    /* Same assertion as above, but with a delay between renders to catch
     * any wall-clock dependency that the first case might miss if the runs
     * happen inside the same jiffy. */
    namespace tb = me::tests::tb;
    const std::string timeline_json = tb::TimelineBuilder()
        .frame_rate(10, 1).resolution(320, 240)
        .add_asset(tb::AssetSpec{.uri = "file://" + fixture_path})
        .add_clip(tb::ClipSpec{
            .time_start_den = 10, .time_dur_num = 10, .time_dur_den = 10,
            .source_start_den = 10, .source_dur_num = 10, .source_dur_den = 10,
        })
        .build();

    const fs::path tmp_dir = fs::temp_directory_path() / "me-determinism-test-2";
    fs::create_directories(tmp_dir);
    const fs::path out1 = tmp_dir / "a.mp4";
    const fs::path out2 = tmp_dir / "b.mp4";
    fs::remove(out1);
    fs::remove(out2);

    REQUIRE(render_passthrough(timeline_json, out1.string()) == ME_OK);

    /* Chew through some cycles so wall-clock advances. doctest doesn't
     * ship a sleep primitive; a tight loop of malloc-free pairs is
     * enough to advance several milliseconds without depending on
     * <thread>. */
    for (int i = 0; i < 100000; ++i) {
        volatile void* p = std::malloc(64);
        std::free(const_cast<void*>(p));
    }

    REQUIRE(render_passthrough(timeline_json, out2.string()) == ME_OK);

    const auto bytes1 = slurp(out1);
    const auto bytes2 = slurp(out2);
    REQUIRE(!bytes1.empty());
    CHECK(bytes1 == bytes2);
}

TEST_CASE("h264/aac reencode is byte-deterministic across two independent renders") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    /* Same fixture, now fed through the h264_videotoolbox + libavcodec-aac
     * reencode path. AVFMT_FLAG_BITEXACT + AV_CODEC_FLAG_BITEXACT are set
     * upstream so mvhd creation_time + encoder version strings don't leak
     * into the output. h264_videotoolbox is a HW encoder and thus "advisory"
     * w.r.t. bit-exactness, but it's stable run-to-run on the same host —
     * this case is a tripwire for both muxer-side metadata regressions and
     * future HW-encoder behavior shifts. */
    namespace tb = me::tests::tb;
    const std::string timeline_json = tb::TimelineBuilder()
        .frame_rate(25, 1).resolution(640, 480)
        .add_asset(tb::AssetSpec{.uri = "file://" + fixture_path})
        .add_clip(tb::ClipSpec{
            .time_start_den = 25, .time_dur_num = 25, .time_dur_den = 25,
            .source_start_den = 25, .source_dur_num = 25, .source_dur_den = 25,
        })
        .build();

    const fs::path tmp_dir = fs::temp_directory_path() / "me-determinism-test-reencode";
    fs::create_directories(tmp_dir);
    const fs::path out1 = tmp_dir / "reenc1.mp4";
    const fs::path out2 = tmp_dir / "reenc2.mp4";
    fs::remove(out1);
    fs::remove(out2);

    /* The reencode path needs a machine with h264_videotoolbox available.
     * When unavailable (non-mac CI), skip rather than hard-fail — the
     * passthrough cases above still provide software-path coverage. */
    const me_status_t s1 = render_with_spec(timeline_json, out1.string(), "h264", "aac");
    if (s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE) {
        MESSAGE("skipping reencode determinism test: h264_videotoolbox unavailable (status="
                << me_status_str(s1) << ")");
        return;
    }
    REQUIRE(s1 == ME_OK);
    REQUIRE(render_with_spec(timeline_json, out2.string(), "h264", "aac") == ME_OK);

    const auto bytes1 = slurp(out1);
    const auto bytes2 = slurp(out2);
    REQUIRE(!bytes1.empty());
    REQUIRE(!bytes2.empty());
    CHECK(bytes1.size() == bytes2.size());
    if (bytes1 != bytes2) {
        size_t i = 0;
        while (i < bytes1.size() && i < bytes2.size() && bytes1[i] == bytes2[i]) ++i;
        FAIL("reencode outputs differ at byte offset " << i);
    } else {
        CHECK(true);
    }
}

TEST_CASE("h264/aac reencode concat across N segments is byte-deterministic") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    /* Two contiguous clips, each 25/25 = 1s of the same fixture. Shared
     * encoder runs across both segments (reencode-multi-clip, 2bfa6cd);
     * BITEXACT flags applied both to the muxer and the encoder (debt-
     * render-bitexact-flags, 4ad072e). Single-clip reencode determinism
     * is already pinned by the preceding TEST_CASE — this one
     * additionally tripwires the multi-segment process_segment loop and
     * the cross-segment next_video_pts / next_audio_pts counters.
     *
     * No audio in determinism_input.mp4 — shared_encoder's aenc stays
     * null and the reencode path degrades to video-only. That's still
     * the correct semantic for fixture with no audio track; h264 path is
     * what we're tripping on regardless. */
    namespace tb = me::tests::tb;
    const std::string timeline_json = tb::TimelineBuilder()
        .frame_rate(25, 1).resolution(640, 480)
        .add_asset(tb::AssetSpec{.uri = "file://" + fixture_path})
        .add_clip(tb::ClipSpec{
            .clip_id = "c1",
            .time_start_num = 0,  .time_start_den = 25,
            .time_dur_num   = 25, .time_dur_den   = 25,
            .source_start_den = 25,
            .source_dur_num = 25, .source_dur_den = 25,
        })
        .add_clip(tb::ClipSpec{
            .clip_id = "c2",
            .time_start_num = 25, .time_start_den = 25,
            .time_dur_num   = 25, .time_dur_den   = 25,
            .source_start_den = 25,
            .source_dur_num = 25, .source_dur_den = 25,
        })
        .build();

    const fs::path tmp_dir =
        fs::temp_directory_path() / "me-determinism-test-reencode-multi";
    fs::create_directories(tmp_dir);
    const fs::path out1 = tmp_dir / "multi1.mp4";
    const fs::path out2 = tmp_dir / "multi2.mp4";
    fs::remove(out1);
    fs::remove(out2);

    const me_status_t s1 = render_with_spec(timeline_json, out1.string(), "h264", "aac");
    if (s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE) {
        MESSAGE("skipping multi-clip reencode determinism test: h264_videotoolbox unavailable "
                "(status=" << me_status_str(s1) << ")");
        return;
    }
    REQUIRE(s1 == ME_OK);
    REQUIRE(render_with_spec(timeline_json, out2.string(), "h264", "aac") == ME_OK);

    const auto bytes1 = slurp(out1);
    const auto bytes2 = slurp(out2);
    REQUIRE(!bytes1.empty());
    REQUIRE(!bytes2.empty());
    CHECK(bytes1.size() == bytes2.size());
    if (bytes1 != bytes2) {
        size_t i = 0;
        while (i < bytes1.size() && i < bytes2.size() && bytes1[i] == bytes2[i]) ++i;
        FAIL("multi-clip reencode outputs differ at byte offset " << i);
    } else {
        CHECK(true);
    }
}

#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO
#define ME_TEST_FIXTURE_MP4_WITH_AUDIO ""
#endif

TEST_CASE("compose path (2-track video + audio mixer) is byte-deterministic across two independent renders") {
    /* Cover the M2 software-path determinism criterion end-to-end:
     * renders a timeline exercising ComposeSink (multi-track) +
     * AudioMixer (explicit audio track) + the AAC encoder twice
     * from fresh engines and compares output bytes. As with the
     * h264/aac reencode case, this leans on h264_videotoolbox's
     * run-to-run stability (same machine, bitexact flags) — not a
     * strict software-determinism guarantee, but it's the tripwire
     * M2 requires for ComposeSink regressions that would perturb
     * the encoder inputs. Skips cleanly when videotoolbox is
     * unavailable (non-mac CI). */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    ME_REQUIRE_FIXTURE(fixture_path);
    const std::string fixture_uri = "file://" + fixture_path;
    const std::string timeline_json = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_vid","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c_aud","assetId":"a1","gainDb":{"static":0},
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    const fs::path tmp_dir = fs::temp_directory_path() / "me-determinism-test-compose";
    fs::create_directories(tmp_dir);
    const fs::path out1 = tmp_dir / "compose1.mp4";
    const fs::path out2 = tmp_dir / "compose2.mp4";
    fs::remove(out1);
    fs::remove(out2);

    const me_status_t s1 = render_with_spec(timeline_json, out1.string(), "h264", "aac");
    if (s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE) {
        MESSAGE("skipping compose determinism test: videotoolbox or encoder unavailable (status="
                << me_status_str(s1) << ")");
        return;
    }
    REQUIRE(s1 == ME_OK);
    REQUIRE(render_with_spec(timeline_json, out2.string(), "h264", "aac") == ME_OK);

    const auto bytes1 = slurp(out1);
    const auto bytes2 = slurp(out2);
    REQUIRE(!bytes1.empty());
    REQUIRE(!bytes2.empty());
    CHECK(bytes1.size() == bytes2.size());
    if (bytes1 != bytes2) {
        size_t i = 0;
        while (i < bytes1.size() && i < bytes2.size() && bytes1[i] == bytes2[i]) ++i;
        FAIL("compose outputs differ at byte offset " << i);
    } else {
        CHECK(true);
    }
}
