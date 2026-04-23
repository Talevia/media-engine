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

namespace fs = std::filesystem;

namespace {

/* Slurp a file into a byte vector. Empty vector on error. */
std::vector<unsigned char> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),
                                       std::istreambuf_iterator<char>());
}

/* Render the given timeline JSON through passthrough to out_path. Returns
 * ME_OK on success; test fails otherwise. Each call builds a fresh engine
 * so we prove determinism across engine instantiations, not just back-to-back
 * renders within a single process. */
me_status_t render_passthrough(const std::string& timeline_json,
                                const std::string& out_path) {
    me_engine_t* eng = nullptr;
    me_status_t s = me_engine_create(nullptr, &eng);
    if (s != ME_OK) return s;

    me_timeline_t* tl = nullptr;
    s = me_timeline_load_json(eng, timeline_json.data(), timeline_json.size(), &tl);
    if (s != ME_OK) { me_engine_destroy(eng); return s; }

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

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

}  // namespace

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

TEST_CASE("passthrough is byte-deterministic across two independent renders") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping determinism test: fixture not available "
                "(build without ffmpeg CLI?): " << fixture_path);
        return;
    }

    /* Timeline with a single passthrough clip referencing the fixture.
     * Fixture is 10 frames at 10 fps = 1 second; timeRange matches. */
    namespace tb = me::tests::tb;
    const std::string timeline_json = tb::TimelineBuilder()
        .frame_rate(10, 1).resolution(320, 240)
        .add_asset(tb::AssetSpec{.uri = "file://" + fixture_path})
        .add_clip(tb::ClipSpec{
            .time_start_den = 10, .time_dur_num = 10, .time_dur_den = 10,
            .source_start_den = 10, .source_dur_num = 10, .source_dur_den = 10,
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
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping determinism test (second case): fixture missing");
        return;
    }

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
