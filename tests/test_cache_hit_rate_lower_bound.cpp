/*
 * test_cache_hit_rate_lower_bound — VISION §5.7-6 guard.
 *
 * §3.3 + §5.7-6 say "same timeline, same time → cache must serve
 * subsequent fetches". `tests/test_frame_server.cpp` already proves
 * hit_count *advances* on scrub-back; what it does NOT prove is a
 * *rate* — a regression that always misses on first attempt and only
 * hits 50% of repeats would still pass the existing "advances"
 * checks but would silently halve scrub responsiveness.
 *
 * This test pins a numerical lower bound: render the *same* frame N
 * times in a row; the post-warmup hit rate must be ≥ kHitRateFloor.
 *
 * Why N repeats of the same time and not a scrub pattern: we want to
 * isolate "does the cache serve a known-keyed repeat?" The disk_cache
 * write-through happens on the first call; calls 2..N must all hit.
 * That gives an expected rate of (N-1)/N → ~0.9 for N=10. The floor
 * is set conservatively (0.5) so a single warm-up miss does not flap
 * the test on dev box noise; tightening to 0.85 is a follow-up once
 * the rate has been observed stable across N CI runs.
 *
 * What this test would catch:
 *   - cache key includes a timestamp / nonce by accident → 0% hits
 *   - DiskCache write-through silently fails → 0% hits
 *   - LRU evicts within a single tight loop → degrading hit rate
 *   - asset_hash_cache reset on each render → 0% hits
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "fixture_skip.hpp"
#include "scratch_dir.hpp"

#include <filesystem>
#include <memory>
#include <string>

#ifndef ME_TEST_FIXTURE_MP4
#error "ME_TEST_FIXTURE_MP4 must be defined via CMake"
#endif

namespace {

constexpr int    kIters         = 10;
constexpr double kHitRateFloor  = 0.5;  /* (N-1)/N expected; floor leaves room
                                           for warmup + minor LRU jitter */

std::string build_single_clip_json(const char* uri) {
    std::string s = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":160,"height":120},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a0","uri":")";
    s += uri;
    s += R"("}],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"video","clips":[
            {"id":"c0","type":"video","assetId":"a0",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "sourceRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    return s;
}

}  // namespace

TEST_CASE("cache hit rate ≥ floor on repeated same-frame renders (§5.7-6)") {
    ME_REQUIRE_FIXTURE(ME_TEST_FIXTURE_MP4);

    /* Need a real cache_dir so DiskCache write-through is exercised;
     * the in-memory AssetHashCache alone wouldn't surface a regression
     * in disk persistence. ScratchDir cleans up on destruction. */
    me::testing::ScratchDir cache_dir{"cache_hit_rate_floor"};
    const std::string cache_dir_str = cache_dir.path.string();

    me_engine_config_t cfg{};
    cfg.cache_dir = cache_dir_str.c_str();

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(&cfg, &eng) == ME_OK);
    struct EngineGuard {
        me_engine_t* p;
        ~EngineGuard() { me_engine_destroy(p); }
    } eg{eng};

    const std::string uri = "file://" + std::string(ME_TEST_FIXTURE_MP4);
    const std::string js  = build_single_clip_json(uri.c_str());
    me_timeline_t* tl = nullptr;
    REQUIRE(me_timeline_load_json(eng, js.data(), js.size(), &tl) == ME_OK);
    struct TimelineGuard {
        me_timeline_t* p;
        ~TimelineGuard() { me_timeline_destroy(p); }
    } tg{tl};

    me_cache_stats_t before{};
    REQUIRE(me_cache_stats(eng, &before) == ME_OK);

    const me_rational_t t = {0, 1};  /* always the same time */
    for (int i = 0; i < kIters; ++i) {
        me_frame_t* f = nullptr;
        const me_status_t s = me_render_frame(eng, tl, t, &f);
        REQUIRE(s == ME_OK);
        REQUIRE(f != nullptr);
        me_frame_destroy(f);
    }

    me_cache_stats_t after{};
    REQUIRE(me_cache_stats(eng, &after) == ME_OK);

    const std::int64_t hits   = after.hit_count   - before.hit_count;
    const std::int64_t misses = after.miss_count  - before.miss_count;
    const std::int64_t total  = hits + misses;
    REQUIRE(total > 0);  /* sanity: cache layer was actually consulted */

    const double rate = static_cast<double>(hits) / static_cast<double>(total);
    INFO("repeated-render cache stats: hits=" << hits << " misses=" << misses
         << " total=" << total << " hit_rate=" << rate
         << " floor=" << kHitRateFloor);

    /* Strong constraint: at least one repeat must serve from cache.
     * If `hits == 0` despite N=10 identical fetches, the cache is
     * effectively dead — that is the regression worth screaming about. */
    CHECK(hits > 0);
    CHECK(rate >= kHitRateFloor);
}
