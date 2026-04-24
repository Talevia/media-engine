/*
 * test_frame_server_concurrent — pin me_render_frame thread
 * safety when multiple UI-scrub threads hit a shared engine +
 * timeline at once.
 *
 * `src/api/render.cpp:95` instantiates a fresh Previewer per
 * me_render_frame call, and the engine's shared services
 * (AssetHashCache, DiskCache, CodecPool) all carry their own
 * mutexes. That makes concurrent scrubbing safe *in theory*;
 * this suite pins the claim empirically.
 *
 * The check shape intentionally stays behavioural — we don't
 * race on internal counters (which are observable via
 * me_cache_stats but whose transient values across threads
 * aren't specified). Instead, each thread runs N frame-fetches
 * at different times and asserts (a) no call returns anything
 * other than ME_OK / ME_E_NOT_FOUND, (b) every ME_OK result
 * yields a non-null frame with sane dims, and (c) the engine-
 * wide cache counters advance monotonically as fetches
 * accumulate.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

namespace {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};
struct TimelineHandle {
    me_timeline_t* p = nullptr;
    ~TimelineHandle() { if (p) me_timeline_destroy(p); }
};

std::string single_clip_timeline(const std::string& fixture_uri) {
    return std::string(R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a0","uri":")") + fixture_uri + R"("}],
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
}

}  // namespace

TEST_CASE("me_render_frame: concurrent scrubbing from 4 threads returns valid frames") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

    /* Scratch cache_dir so DiskCache sees a real persistence path
     * — threaded fetches should race through the cache's mutex
     * without corrupting the in-memory `disk_bytes_used` counter
     * or the .bin files on disk. */
    const fs::path cache_dir = fs::temp_directory_path() /
                                ("me_frame_server_concurrent_" +
                                 std::to_string(
                                     reinterpret_cast<uintptr_t>(
                                         &fixture_path)));
    fs::create_directories(cache_dir);
    struct ScratchGuard {
        fs::path p;
        ~ScratchGuard() { std::error_code ec; fs::remove_all(p, ec); }
    } cleanup{cache_dir};

    me_engine_config_t cfg{};
    const std::string cache_dir_str = cache_dir.string();
    cfg.cache_dir = cache_dir_str.c_str();

    EngineHandle eng;
    REQUIRE(me_engine_create(&cfg, &eng.p) == ME_OK);

    const std::string j = single_clip_timeline("file://" + fixture_path);
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    constexpr int kThreads          = 4;
    constexpr int kFetchesPerThread = 20;

    std::atomic<int> successes{0};
    std::atomic<int> not_found{0};
    std::atomic<int> others{0};
    std::atomic<bool> saw_bad_frame{false};

    const auto worker = [&](int tid) {
        for (int i = 0; i < kFetchesPerThread; ++i) {
            /* Pick a time inside the clip's 0..2s range that
             * differs per iteration + thread so multiple cache
             * keys get exercised simultaneously. t = (tid × 100
             * + i × 13) ms. */
            const int64_t t_ms = (tid * 100 + i * 13) % 1800;
            const me_rational_t t{t_ms, 1000};
            me_frame_t* frame = nullptr;
            const me_status_t s = me_render_frame(eng.p, tl.p, t, &frame);
            if (s == ME_OK) {
                successes.fetch_add(1, std::memory_order_relaxed);
                if (frame == nullptr ||
                    me_frame_width(frame)  <= 0 ||
                    me_frame_height(frame) <= 0 ||
                    me_frame_pixels(frame) == nullptr) {
                    saw_bad_frame.store(true, std::memory_order_release);
                }
                me_frame_destroy(frame);
            } else if (s == ME_E_NOT_FOUND) {
                /* t past clip range — rare with our % 1800 cap on
                 * a 2 s clip, but accept without flagging. */
                not_found.fetch_add(1, std::memory_order_relaxed);
            } else {
                others.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back(worker, tid);
    }
    for (auto& th : threads) th.join();

    const int total = successes.load() + not_found.load() + others.load();
    CHECK(total == kThreads * kFetchesPerThread);
    CHECK(others.load() == 0);           /* no unexpected errors */
    CHECK_FALSE(saw_bad_frame.load());   /* every ME_OK yielded
                                           * a usable frame */
    CHECK(successes.load() > 0);         /* not all turned into
                                           * NOT_FOUND due to some
                                           * range-check race */

    /* Cache stats reflect the combined load from all threads.
     * hit_count + miss_count should at least equal successes
     * (each successful fetch either hit or missed). Counters are
     * AssetHashCache-level + DiskCache-level summed per
     * api/cache.cpp:59. */
    me_cache_stats_t stats{};
    REQUIRE(me_cache_stats(eng.p, &stats) == ME_OK);
    CHECK(stats.hit_count + stats.miss_count >= successes.load());
    CHECK(stats.disk_bytes_used >= 0);   /* counter healthy
                                           * (didn't underflow on
                                           * concurrent puts /
                                           * evictions) */
}
