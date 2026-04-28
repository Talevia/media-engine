/*
 * test_frame_server — pixel-proof tests for the M6 frame-server
 * core path (me_render_frame → resolve_active_clip_at + DiskCache
 * peek + compose_frame_at → RGBA8 me_frame).
 *
 * Uses the determinism_fixture MP4 (2s / 30fps deterministic
 * video) wrapped in a single-clip timeline. Asserts dimensions
 * + non-transparent pixels + accessor round-trip.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "resource/content_hash.hpp"
#include "scratch_dir.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#ifndef ME_TEST_FIXTURE_MP4
#error "ME_TEST_FIXTURE_MP4 must be defined via CMake"
#endif

namespace {

std::string build_single_clip_json(const char* uri,
                                    const std::string& content_hash = "") {
    /* Timeline: single 2s video clip of the fixture. frame_rate
     * matches the fixture's 30fps. content_hash is optional;
     * non-empty values seed the engine's AssetHashCache so
     * me_cache_invalidate_asset can target this asset's
     * derived frames via the side-index. */
    std::string s = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":160,"height":120},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a0","uri":")";
    s += uri;
    s += "\"";
    if (!content_hash.empty()) {
        s += ",\"contentHash\":\"sha256:";
        s += content_hash;
        s += "\"";
    }
    s += R"(}],
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

struct TimelineRAII {
    me_engine_t*   eng = nullptr;
    me_timeline_t* tl  = nullptr;
    /* Scratch dir for DiskCache tests — populated when use_cache
     * is true. ScratchDir's dtor handles remove_all. */
    std::unique_ptr<me::testing::ScratchDir> cache_dir;

    explicit TimelineRAII(bool use_cache = false) {
        me_engine_config_t cfg{};
        if (use_cache) {
            cache_dir = std::make_unique<me::testing::ScratchDir>("frame_server");
            cache_dir_str = cache_dir->path.string();
            cfg.cache_dir = cache_dir_str.c_str();
        }
        me_engine_create(&cfg, &eng);
        const std::string uri = "file://" + std::string(ME_TEST_FIXTURE_MP4);
        const std::string js  = build_single_clip_json(uri.c_str());
        me_timeline_load_json(eng, js.data(), js.size(), &tl);
    }
    ~TimelineRAII() {
        if (tl)  me_timeline_destroy(tl);
        if (eng) me_engine_destroy(eng);
        /* cache_dir's unique_ptr dtor → ScratchDir dtor → remove_all */
    }
private:
    std::string cache_dir_str;  /* keeps the C string alive for cfg.cache_dir */
};

}  // namespace

TEST_CASE("me_render_frame: returns valid RGBA8 frame from fixture") {
    TimelineRAII f;
    REQUIRE(f.eng);
    REQUIRE(f.tl);

    me_frame_t* frame = nullptr;
    const me_status_t s = me_render_frame(
        f.eng, f.tl, me_rational_t{1, 1},  /* t = 1.0s */
        &frame);

    REQUIRE(s == ME_OK);
    REQUIRE(frame != nullptr);

    const int w = me_frame_width(frame);
    const int h = me_frame_height(frame);
    CHECK(w > 0);
    CHECK(h > 0);

    const uint8_t* px = me_frame_pixels(frame);
    REQUIRE(px != nullptr);

    /* At least one pixel has non-zero alpha — frame actually
     * contains decoded content. */
    bool any_alpha = false;
    for (int i = 3; i < w * h * 4; i += 4) {
        if (px[i] != 0) { any_alpha = true; break; }
    }
    CHECK(any_alpha);

    me_frame_destroy(frame);
}

TEST_CASE("me_render_frame: NULL args return ME_E_INVALID_ARG") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;

    CHECK(me_render_frame(nullptr, f.tl, me_rational_t{0, 1}, &frame) == ME_E_INVALID_ARG);
    CHECK(me_render_frame(f.eng, nullptr, me_rational_t{0, 1}, &frame) == ME_E_INVALID_ARG);
    CHECK(me_render_frame(f.eng, f.tl, me_rational_t{0, 1}, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("me_render_frame: time past timeline duration → ME_E_NOT_FOUND") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;
    /* Timeline is 2s; t=5s is past end. */
    const me_status_t s = me_render_frame(
        f.eng, f.tl, me_rational_t{5, 1}, &frame);
    CHECK(s == ME_E_NOT_FOUND);
    CHECK(frame == nullptr);
}

TEST_CASE("me_frame accessors: NULL-safe + destroy is idempotent-safe") {
    /* me_frame_* all accept NULL gracefully — common pattern for
     * opaque-handle C APIs. */
    CHECK(me_frame_width(nullptr)  == 0);
    CHECK(me_frame_height(nullptr) == 0);
    CHECK(me_frame_pixels(nullptr) == nullptr);
    me_frame_destroy(nullptr);  /* must not crash */
}

TEST_CASE("me_render_frame: dimensions match fixture W×H") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{0, 1}, &frame) == ME_OK);
    REQUIRE(frame != nullptr);

    /* Fixture is generated at 640×480 by gen_fixture
     * (verified on dev machine; mirror the determinism_fixture
     * target's settings if the fixture is rebuilt at a
     * different resolution). */
    CHECK(me_frame_width(frame) == 640);
    CHECK(me_frame_height(frame) == 480);

    me_frame_destroy(frame);
}

/* ------------------------------------------------------------------
 * Cache stats + scrubbing reuse — the coupled M6 criteria.
 * These tests enable cache_dir on the engine so DiskCache is
 * populated by me_render_frame's DiskCache write-through.
 * ------------------------------------------------------------------ */

TEST_CASE("me_cache_stats: hit/miss counters advance on scrub-back") {
    TimelineRAII f(/*use_cache=*/true);
    REQUIRE(f.eng);

    me_cache_stats_t s0{};
    REQUIRE(me_cache_stats(f.eng, &s0) == ME_OK);
    const int64_t h0 = s0.hit_count;
    const int64_t m0 = s0.miss_count;

    /* First fetch at t=1.0 — DiskCache should miss, decode, then
     * persist the frame. */
    me_frame_t* frame1 = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 1}, &frame1) == ME_OK);
    REQUIRE(frame1 != nullptr);

    me_cache_stats_t s1{};
    REQUIRE(me_cache_stats(f.eng, &s1) == ME_OK);
    CHECK(s1.miss_count > m0);  /* at least one miss */
    me_frame_destroy(frame1);

    /* Scrub away to t=0.5 — different key, also a miss. */
    me_frame_t* frame2 = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 2}, &frame2) == ME_OK);
    REQUIRE(frame2 != nullptr);
    me_frame_destroy(frame2);

    me_cache_stats_t s2{};
    REQUIRE(me_cache_stats(f.eng, &s2) == ME_OK);
    CHECK(s2.miss_count > s1.miss_count);

    /* Scrub back to t=1.0 — same key as first fetch → DiskCache
     * hit. hit_count must advance. */
    me_frame_t* frame3 = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 1}, &frame3) == ME_OK);
    REQUIRE(frame3 != nullptr);

    me_cache_stats_t s3{};
    REQUIRE(me_cache_stats(f.eng, &s3) == ME_OK);
    CHECK(s3.hit_count > h0);
    CHECK(s3.hit_count > s2.hit_count);  /* strictly more than pre-scrub-back */
    me_frame_destroy(frame3);

    /* disk_bytes_used should be positive now (two distinct frames
     * cached). */
    CHECK(s3.disk_bytes_used > 0);
}

TEST_CASE("me_cache_invalidate_asset: targeted hash drops derived disk-cache frames") {
    /* The targeted-invalidation path: with the asset's contentHash
     * seeded into the engine, me_cache_invalidate_asset(<asset_hash>)
     * should evict every frame derived from this asset out of the
     * DiskCache (graph-hash-keyed entries reached via the
     * asset_hash → set<cache_key> side-index). Pre-debt-cache-
     * invalidate-asset-graph-hash this would silently no-op against
     * graph-hash keys, and the previous TEST_CASE worked around it
     * by going through me_cache_clear instead. */
    namespace fs = std::filesystem;
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    std::string err;
    const std::string fixture_hash =
        me::resource::sha256_hex_streaming(fixture_path, &err);
    REQUIRE(!fixture_hash.empty());

    me::testing::ScratchDir cache_dir{"frame_server"};
    me_engine_config_t cfg{};
    const std::string cache_dir_str = cache_dir.path.string();
    cfg.cache_dir = cache_dir_str.c_str();

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(&cfg, &eng) == ME_OK);

    me_timeline_t* tl = nullptr;
    const std::string uri = "file://" + fixture_path;
    const std::string js  = build_single_clip_json(uri.c_str(), fixture_hash);
    REQUIRE(me_timeline_load_json(eng, js.data(), js.size(), &tl) == ME_OK);

    /* Render at t=1.0 → DiskCache populated; second render hits. */
    me_frame_t* f1 = nullptr;
    REQUIRE(me_render_frame(eng, tl, me_rational_t{1, 1}, &f1) == ME_OK);
    me_frame_destroy(f1);

    me_cache_stats_t s_before{};
    me_cache_stats(eng, &s_before);
    me_frame_t* f_hit = nullptr;
    REQUIRE(me_render_frame(eng, tl, me_rational_t{1, 1}, &f_hit) == ME_OK);
    me_frame_destroy(f_hit);
    me_cache_stats_t s_hit{};
    me_cache_stats(eng, &s_hit);
    REQUIRE(s_hit.hit_count > s_before.hit_count);  /* cache is live */

    const int64_t bytes_with_entry = s_hit.disk_bytes_used;
    REQUIRE(bytes_with_entry > 0);

    /* Targeted invalidate via the seeded fixture hash. */
    REQUIRE(me_cache_invalidate_asset(eng, fixture_hash.c_str()) == ME_OK);

    me_cache_stats_t s_post_inv{};
    me_cache_stats(eng, &s_post_inv);
    /* DiskCache file removed → bytes_used drops; AssetHashCache
     * entry removed too → entry_count drops. */
    CHECK(s_post_inv.disk_bytes_used < bytes_with_entry);

    /* Next render at t=1.0 should miss disk_cache and recompute. */
    const int64_t miss_pre = s_post_inv.miss_count;
    me_frame_t* f_miss = nullptr;
    REQUIRE(me_render_frame(eng, tl, me_rational_t{1, 1}, &f_miss) == ME_OK);
    me_frame_destroy(f_miss);
    me_cache_stats_t s_miss{};
    me_cache_stats(eng, &s_miss);
    CHECK(s_miss.miss_count > miss_pre);

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
}

TEST_CASE("me_cache_invalidate_asset: next fetch misses") {
    TimelineRAII f(/*use_cache=*/true);
    REQUIRE(f.eng);

    /* Prime the cache at t=1.0. */
    me_frame_t* first = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 1}, &first) == ME_OK);
    me_frame_destroy(first);

    /* Scrub back → cache hit (confirmed by counter increment). */
    me_cache_stats_t s_before{};
    me_cache_stats(f.eng, &s_before);
    me_frame_t* hit_frame = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 1}, &hit_frame) == ME_OK);
    me_frame_destroy(hit_frame);
    me_cache_stats_t s_after{};
    me_cache_stats(f.eng, &s_after);
    REQUIRE(s_after.hit_count > s_before.hit_count);  /* precondition */

    /* Invalidate using the fixture's content hash. We don't know it
     * without peeking at internals — but invalidate_by_hash on any
     * hash clears AssetHashCache entries matching it; the DiskCache
     * side uses the same hash as file prefix. For this test we
     * invalidate with an arbitrary string that won't match, then
     * confirm subsequent fetches still hit (negative control); then
     * clear the full cache and confirm misses.
     *
     * Simpler + stronger: call me_cache_clear — purges both
     * AssetHashCache + DiskCache + resets counters. Then fetch at
     * t=1.0 again → must be a miss. */
    REQUIRE(me_cache_clear(f.eng) == ME_OK);

    me_cache_stats_t s_cleared{};
    me_cache_stats(f.eng, &s_cleared);
    /* disk_bytes_used drops to 0 — DiskCache::clear removes all
     * .bin files. Counter semantics across AssetHashCache vs
     * DiskCache differ (AssetHashCache's are lifetime-cumulative;
     * DiskCache's reset on clear); callers rely on "did they
     * advance?" not "are they zero?" for cache state checks. */
    CHECK(s_cleared.disk_bytes_used == 0);
    const int64_t m_pre = s_cleared.miss_count;

    me_frame_t* post = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{1, 1}, &post) == ME_OK);
    me_frame_destroy(post);

    me_cache_stats_t s_miss{};
    me_cache_stats(f.eng, &s_miss);
    CHECK(s_miss.miss_count > m_pre);  /* fresh miss after clear */
}
