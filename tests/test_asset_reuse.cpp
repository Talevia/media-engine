/*
 * test_asset_reuse — asserts the `timeline-asset-map` dedup contract.
 *
 * Background: the `timeline-asset-map` decision (2026-04-23) promoted
 * `me::Asset` to a first-class IR node under `me::Timeline::assets` so
 * multiple clips referencing the same `assetId` share a single Asset
 * entry (URI + content_hash live on the Asset, not duplicated per clip).
 * That contract is strictly observable via `me_cache_stats.entry_count`:
 * two clips of the same asset must register **one** cache entry, not
 * two. Without this tripwire, a future Exporter refactor that rebuilt
 * the asset map from `clips[].asset_id` (pre-decision shape) would
 * silently regress to per-clip duplication.
 *
 * No decoder/encoder/mux invocation — load + stats is enough to pin the
 * IR-level guarantee. Future cycles can extend with a demux-level reuse
 * assertion (same URI → single DemuxContext) once the Exporter is
 * instrumented for that.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <string>

namespace {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};

struct TimelineHandle {
    me_timeline_t* p = nullptr;
    ~TimelineHandle() { if (p) me_timeline_destroy(p); }
};

namespace tb = me::tests::tb;

/* Two clips, each carrying an explicit `contentHash` so the cache has
 * something concrete to count. `me_cache_stats.entry_count` tracks
 * sha256-indexed assets, so the hash is what makes the assertion
 * stable rather than encoder/decoder behaviour. */
/* Loader enforces phase-1 constraints: `timeRange.start` of clip[i]
 * equals cumulative prior duration (no gaps/overlaps), and
 * `timeRange.duration == sourceRange.duration` (no speed change).
 * Both clips below run 1 second each (30 frames @ 30fps), laid back-
 * to-back starting at t=0. */
constexpr int kDen    = 30;
constexpr int kClip1  = 30;          /* first clip: 0 .. 30/30 = 1s */
constexpr int kClip2Start = kClip1;
constexpr int kClip2  = 30;          /* second clip: 1s .. 2s */

std::string build_two_clip_same_asset_json(const std::string& uri,
                                            const std::string& hex_hash) {
    return tb::TimelineBuilder()
        .add_asset(tb::AssetSpec{
            .id = "a1",
            .uri = uri,
            .content_hash = "sha256:" + hex_hash,
        })
        .add_clip(tb::ClipSpec{
            .clip_id = "c1", .asset_id = "a1",
            .time_start_num = 0,            .time_start_den = kDen,
            .time_dur_num   = kClip1,       .time_dur_den   = kDen,
            .source_dur_num = kClip1,       .source_dur_den = kDen,
        })
        .add_clip(tb::ClipSpec{
            .clip_id = "c2", .asset_id = "a1",
            .time_start_num = kClip2Start,  .time_start_den = kDen,
            .time_dur_num   = kClip2,       .time_dur_den   = kDen,
            .source_dur_num = kClip2,       .source_dur_den = kDen,
        })
        .build();
}

std::string build_two_clip_two_assets_json(const std::string& uri_a,
                                            const std::string& hash_a,
                                            const std::string& uri_b,
                                            const std::string& hash_b) {
    return tb::TimelineBuilder()
        .add_asset(tb::AssetSpec{
            .id = "a1", .uri = uri_a, .content_hash = "sha256:" + hash_a,
        })
        .add_asset(tb::AssetSpec{
            .id = "a2", .uri = uri_b, .content_hash = "sha256:" + hash_b,
        })
        .add_clip(tb::ClipSpec{
            .clip_id = "c1", .asset_id = "a1",
            .time_start_num = 0,            .time_start_den = kDen,
            .time_dur_num   = kClip1,       .time_dur_den   = kDen,
            .source_dur_num = kClip1,       .source_dur_den = kDen,
        })
        .add_clip(tb::ClipSpec{
            .clip_id = "c2", .asset_id = "a2",
            .time_start_num = kClip2Start,  .time_start_den = kDen,
            .time_dur_num   = kClip2,       .time_dur_den   = kDen,
            .source_dur_num = kClip2,       .source_dur_den = kDen,
        })
        .build();
}

}  // namespace

TEST_CASE("two clips referencing same asset register one cache entry") {
    /* Fixed, known-unique hash values so reruns don't interfere with
     * each other's cache state. sha256 of "clip-reuse-test" — arbitrary
     * but stable. */
    const std::string hash =
        "5d41402abc4b2a76b9719d911017c592c3a1f0e8adf2eb0a0f8b5c7f3ec6f3a9";

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string json = build_two_clip_same_asset_json(
        "file:///tmp/asset-reuse-shared.mp4", hash);

    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, json.data(), json.size(), &tl.p) == ME_OK);

    me_cache_stats_t stats{};
    REQUIRE(me_cache_stats(eng.p, &stats) == ME_OK);

    /* The timeline-asset-map contract: regardless of clip count, each
     * distinct asset registers exactly one cache entry. */
    CHECK(stats.entry_count == 1);
}

TEST_CASE("two clips referencing two distinct assets register two entries") {
    const std::string hash_a =
        "aa41402abc4b2a76b9719d911017c592c3a1f0e8adf2eb0a0f8b5c7f3ec6f3a9";
    const std::string hash_b =
        "bb41402abc4b2a76b9719d911017c592c3a1f0e8adf2eb0a0f8b5c7f3ec6f3a9";

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string json = build_two_clip_two_assets_json(
        "file:///tmp/asset-reuse-a.mp4", hash_a,
        "file:///tmp/asset-reuse-b.mp4", hash_b);

    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, json.data(), json.size(), &tl.p) == ME_OK);

    me_cache_stats_t stats{};
    REQUIRE(me_cache_stats(eng.p, &stats) == ME_OK);

    /* Two distinct assets → two entries. Symmetric counterpart to the
     * single-asset case; makes the dedup contract testable as a
     * difference (same vs different) rather than just "≤ 1". */
    CHECK(stats.entry_count == 2);
}

TEST_CASE("same-asset timeline's codec_ctx_count stays 0 through load") {
    /* Loader doesn't open decoders — it just parses JSON and seeds
     * asset hashes. codec_ctx_count should only move when a
     * renderer / probe / thumbnail allocates a decoder. Lock this down
     * so a future refactor that starts pre-opening decoders in the
     * loader is caught immediately. */
    const std::string hash =
        "cc41402abc4b2a76b9719d911017c592c3a1f0e8adf2eb0a0f8b5c7f3ec6f3a9";

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string json = build_two_clip_same_asset_json(
        "file:///tmp/asset-reuse-nodecode.mp4", hash);

    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, json.data(), json.size(), &tl.p) == ME_OK);

    me_cache_stats_t stats{};
    REQUIRE(me_cache_stats(eng.p, &stats) == ME_OK);
    CHECK(stats.codec_ctx_count == 0);
}
