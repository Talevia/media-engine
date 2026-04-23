/*
 * test_render_progress — contract tripwire for me_progress_event_t ordering.
 *
 * Surface asserted (beyond what test_render_cancel covers loosely):
 *
 *   - Normal path:  [STARTED (×1, first), FRAMES (×N), COMPLETED (×1, last)].
 *                   COMPLETED carries non-null output_path == spec.path.
 *                   No FAILED anywhere.
 *   - Cancel path:  [STARTED (×1, first), FRAMES (×N), FAILED (×1, last)].
 *                   FAILED.status == ME_E_CANCELLED. No COMPLETED.
 *   - FRAMES ratio is in [0, 1] and monotonically non-decreasing.
 *   - me_render_start accepts NULL cb (no crash, render completes normally).
 *
 * Host UI contract: STARTED initializes the progress bar, FRAMES drives it
 * forward, exactly one terminal (COMPLETED or FAILED) closes it. Refactors
 * in the exporter worker thread have two historical foot-guns this suite
 * pins down — emitting a terminal event before STARTED (host hides UI
 * before showing it), or failing to emit a terminal event at all (host
 * hangs forever). `grep -rn 'ME_PROGRESS_' tests/` before this suite
 * only hit test_render_cancel's loose boolean checks; the ordering +
 * cardinality + ratio monotonicity + output_path presence all lacked
 * a tripwire.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
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

struct JobHandle {
    me_render_job_t* p = nullptr;
    ~JobHandle() { if (p) me_render_job_destroy(p); }
};

/* Thread-safe snapshot of a progress event. We can't retain `output_path`
 * / `message` pointers (lifetime is callback scope per the C API); the
 * callback copies the status + ratio into the vector and, separately,
 * for COMPLETED captures whether output_path matched the expected path
 * at callback time (the only observation that matters for the contract). */
struct EventSnap {
    me_progress_kind_t kind;
    float              ratio;
    me_status_t        status;
    bool               output_path_matched;  /* only meaningful for COMPLETED */
};

struct ProgressLog {
    std::mutex               mtx;
    std::vector<EventSnap>   events;
    std::string              expected_output_path;  /* set before render, read in cb */
    std::atomic<bool>        terminal_seen{false};
};

void on_progress(const me_progress_event_t* ev, void* user) {
    auto* log = static_cast<ProgressLog*>(user);
    EventSnap s{};
    s.kind   = ev->kind;
    s.ratio  = ev->ratio;
    s.status = ev->status;
    if (ev->kind == ME_PROGRESS_COMPLETED && ev->output_path) {
        s.output_path_matched =
            (log->expected_output_path == ev->output_path);
    }
    {
        std::lock_guard<std::mutex> lk(log->mtx);
        log->events.push_back(s);
    }
    if (ev->kind == ME_PROGRESS_COMPLETED || ev->kind == ME_PROGRESS_FAILED) {
        log->terminal_seen.store(true, std::memory_order_release);
    }
}

std::string single_clip_timeline(const std::string& fixture_path) {
    /* Minimal single-clip passthrough timeline against the shared fixture
     * (640×480 @ 25fps, 1s). Passthrough stream-copy produces the full
     * STARTED → FRAMES* → COMPLETED trace quickly on any host. */
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = "file://" + fixture_path});
    b.add_clip(tb::ClipSpec{
        .clip_id = "c1",
        .time_start_num = 0, .time_start_den = 25,
        .time_dur_num   = 25, .time_dur_den   = 25,
        .source_start_den = 25,
        .source_dur_num = 25, .source_dur_den = 25,
    });
    return b.build();
}

std::string three_clip_timeline(const std::string& fixture_path) {
    /* 3-clip reencode timeline reused from test_render_cancel: reencode
     * work takes long enough that `sleep(200ms) → cancel` reliably lands
     * mid-render on dev hardware. */
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = "file://" + fixture_path});
    for (int i = 0; i < 3; ++i) {
        b.add_clip(tb::ClipSpec{
            .clip_id = "c" + std::to_string(i + 1),
            .time_start_num = i * 25, .time_start_den = 25,
            .time_dur_num   = 25,     .time_dur_den   = 25,
            .source_start_den = 25,
            .source_dur_num = 25, .source_dur_den = 25,
        });
    }
    return b.build();
}

}  // namespace

TEST_CASE("Normal-path progress: [STARTED, FRAMES*, COMPLETED] with monotonic ratio") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping progress test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-progress-test";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "normal.mp4";
    fs::remove(out_path);

    ProgressLog log;
    log.expected_output_path = out_path.string();

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, on_progress, &log, &job.p) == ME_OK);
    CHECK(me_render_wait(job.p) == ME_OK);

    std::vector<EventSnap> events;
    {
        std::lock_guard<std::mutex> lk(log.mtx);
        events = log.events;
    }

    /* Cardinality (FRAMES count isn't constrained — different sinks emit
     * different granularities; ordering assertions below cover the shape). */
    size_t started = 0, completed = 0, failed = 0;
    for (const auto& ev : events) {
        if (ev.kind == ME_PROGRESS_STARTED)   ++started;
        if (ev.kind == ME_PROGRESS_COMPLETED) ++completed;
        if (ev.kind == ME_PROGRESS_FAILED)    ++failed;
    }
    CHECK(started == 1);
    CHECK(completed == 1);
    CHECK(failed == 0);

    /* Ordering: STARTED is first, COMPLETED is last, every FRAMES sits
     * strictly between them. */
    REQUIRE(!events.empty());
    CHECK(events.front().kind == ME_PROGRESS_STARTED);
    CHECK(events.back().kind == ME_PROGRESS_COMPLETED);

    /* COMPLETED carries the output_path matching spec.path */
    CHECK(events.back().output_path_matched);

    /* Ratio monotonicity: across all FRAMES events in order, ratio is in
     * [0, 1] and non-decreasing. */
    float last_ratio = 0.0f;
    for (const auto& ev : events) {
        if (ev.kind != ME_PROGRESS_FRAMES) continue;
        CHECK(ev.ratio >= 0.0f);
        CHECK(ev.ratio <= 1.0f);
        CHECK(ev.ratio >= last_ratio);
        last_ratio = ev.ratio;
    }
}

TEST_CASE("Cancel-path progress: [STARTED, FRAMES*, FAILED(CANCELLED)], no COMPLETED") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping cancel-sequence test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = three_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-progress-test";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "cancel.mp4";
    fs::remove(out_path);

    ProgressLog log;
    log.expected_output_path = out_path.string();

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, on_progress, &log, &job.p) == ME_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(me_render_cancel(job.p) == ME_OK);

    const me_status_t wait_s = me_render_wait(job.p);
    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        MESSAGE("skipping cancel-sequence assertion: h264_videotoolbox unavailable");
        return;
    }
    CHECK(wait_s == ME_E_CANCELLED);

    std::vector<EventSnap> events;
    {
        std::lock_guard<std::mutex> lk(log.mtx);
        events = log.events;
    }

    size_t started = 0, completed = 0, failed_cancelled = 0, failed_other = 0;
    for (const auto& ev : events) {
        if (ev.kind == ME_PROGRESS_STARTED)   ++started;
        if (ev.kind == ME_PROGRESS_COMPLETED) ++completed;
        if (ev.kind == ME_PROGRESS_FAILED) {
            if (ev.status == ME_E_CANCELLED) ++failed_cancelled;
            else                              ++failed_other;
        }
    }
    CHECK(started == 1);
    CHECK(completed == 0);
    CHECK(failed_cancelled == 1);
    CHECK(failed_other == 0);

    REQUIRE(!events.empty());
    CHECK(events.front().kind == ME_PROGRESS_STARTED);
    CHECK(events.back().kind == ME_PROGRESS_FAILED);
    CHECK(events.back().status == ME_E_CANCELLED);
}

TEST_CASE("me_render_start accepts NULL progress callback") {
    /* Guard: host code that doesn't need progress updates can pass NULL
     * for both cb and user. Implementation must not deref cb before
     * checking, and must still run the render to completion. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping null-cb test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path out_path = fs::temp_directory_path() / "me-progress-null-cb.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p) == ME_OK);
    CHECK(me_render_wait(job.p) == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);
}
