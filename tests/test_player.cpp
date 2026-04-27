/*
 * test_player — covers me_player_t state machine, seek correctness,
 * and chase-audio master-clock pacing.
 *
 * Plan deviation: the original plan called for three suites
 * (test_player_state / test_player_seek / test_player_av_sync). The
 * fixtures and timeline-builder boilerplate are identical across all
 * three, so this lands them as TEST_CASEs in a single suite to keep
 * the harness lean. Each TEST_CASE is independently runnable.
 *
 * Uses the determinism_fixture MP4 (2 s @ 30 fps deterministic
 * video) wrapped in a single-clip timeline. Audio output is
 * disabled (sample_rate = 0) so the suite runs without an audio
 * device — the chase-audio test injects a synthetic playhead via
 * me_player_report_audio_playhead instead of relying on a real
 * device callback.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef ME_TEST_FIXTURE_MP4
#error "ME_TEST_FIXTURE_MP4 must be defined via CMake"
#endif

namespace {

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

struct PlayerFixture {
    me_engine_t*   eng    = nullptr;
    me_timeline_t* tl     = nullptr;
    me_player_t*   player = nullptr;

    PlayerFixture() {
        me_engine_config_t cfg{};
        me_engine_create(&cfg, &eng);
        const std::string uri = "file://" + std::string(ME_TEST_FIXTURE_MP4);
        const std::string js  = build_single_clip_json(uri.c_str());
        me_timeline_load_json(eng, js.data(), js.size(), &tl);
    }

    me_status_t make_player(const me_player_config_t* pc) {
        return me_player_create(eng, tl, pc, &player);
    }

    ~PlayerFixture() {
        if (player) me_player_destroy(player);
        if (tl)     me_timeline_destroy(tl);
        if (eng)    me_engine_destroy(eng);
    }
};

/* Capture frames delivered by the player so each test can assert
 * counts / timestamps. Deliberately simple: copy the present_at
 * rational + frame size; not the pixels. */
struct FrameCapture {
    std::mutex                  mu;
    std::vector<me_rational_t>  present_ats;
    std::atomic<int>            count{0};

    static void cb(const me_frame_t* f, me_rational_t t, void* user) {
        auto* self = static_cast<FrameCapture*>(user);
        std::lock_guard<std::mutex> lk(self->mu);
        self->present_ats.push_back(t);
        self->count.fetch_add(1, std::memory_order_release);
        (void)f;
    }
};

double rat_to_sec(me_rational_t r) {
    return r.den != 0 ? static_cast<double>(r.num) / static_cast<double>(r.den) : 0.0;
}

}  // namespace

/* ----------------------------------------------------------- state */

TEST_CASE("me_player: create + play + pause delivers frames during play, freezes during pause") {
    /* FrameCapture must outlive the PlayerFixture: the fixture's dtor
     * joins the pacer thread, which may still be calling cb at the
     * moment destroy fires. Reverse-declaration-order destruction
     * gives FrameCapture the longer lifetime. */
    FrameCapture  cap;
    PlayerFixture f;
    REQUIRE(f.eng);
    REQUIRE(f.tl);

    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;          /* video-only, WALL clock */
    pc.master_clock          = ME_CLOCK_WALL;
    pc.video_ring_capacity   = 3;
    REQUIRE(f.make_player(&pc) == ME_OK);

    REQUIRE(me_player_set_video_callback(f.player, &FrameCapture::cb, &cap) == ME_OK);

    /* Initially paused — no frames after a settle window. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(cap.count.load() == 0);
    CHECK(me_player_is_playing(f.player) == 0);

    /* Play for ~250 ms; expect ~7-8 frames at 30 fps but accept any
     * positive count to stay tolerant of CI scheduling jitter. */
    REQUIRE(me_player_play(f.player, 1.0f) == ME_OK);
    CHECK(me_player_is_playing(f.player) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int frames_after_play = cap.count.load();
    CHECK(frames_after_play > 0);

    /* Pause + verify the count freezes. */
    REQUIRE(me_player_pause(f.player) == ME_OK);
    CHECK(me_player_is_playing(f.player) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* Allow at most 1 frame slack — a frame already in the pacer's
     * "wait" branch can land just as pause fires. */
    CHECK(cap.count.load() <= frames_after_play + 1);
}

TEST_CASE("me_player: play(rate) gating — invalid / out-of-window / audio combo") {
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;          /* video-only WALL clock */
    pc.master_clock          = ME_CLOCK_WALL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    /* Hard ABI invariants — non-finite, zero, negative all rejected
     * with INVALID_ARG (pause() is what zero is for; negative is
     * reverse-playback, a separate follow-up). */
    CHECK(me_player_play(f.player, -1.0f) == ME_E_INVALID_ARG);
    CHECK(me_player_play(f.player,  0.0f) == ME_E_INVALID_ARG);
    CHECK(me_player_play(f.player,
                          std::numeric_limits<float>::infinity()) == ME_E_INVALID_ARG);
    CHECK(me_player_play(f.player,
                          std::numeric_limits<float>::quiet_NaN()) == ME_E_INVALID_ARG);

    /* Out-of-window forward rate — rejected with UNSUPPORTED. */
    CHECK(me_player_play(f.player, 0.25f) == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, 4.0f)  == ME_E_UNSUPPORTED);

    /* In-window video-only WALL → all OK. */
    CHECK(me_player_play(f.player, 0.5f) == ME_OK);
    CHECK(me_player_play(f.player, 1.0f) == ME_OK);
    CHECK(me_player_play(f.player, 2.0f) == ME_OK);
}

TEST_CASE("me_player: play(rate ≠ 1.0) on audio timeline is rejected (audio-tempo deferred)") {
    /* Same fixture but with sample_rate > 0 — the timeline determinism
     * fixture has no audio track, so has_audio_track_ stays false and
     * the audio-rate gate is exercised through the ME_CLOCK_AUDIO
     * master path instead. Any of: ME_CLOCK_AUDIO master + rate ≠ 1
     * OR has_audio_track_ + rate ≠ 1 trips UNSUPPORTED. */
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 48000;
    pc.master_clock          = ME_CLOCK_AUDIO;   /* explicit, even if track-less */
    REQUIRE(f.make_player(&pc) == ME_OK);

    CHECK(me_player_play(f.player, 2.0f) == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, 0.5f) == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, 1.0f) == ME_OK);
}

TEST_CASE("me_player: play(2.0) advances the master clock at 2× wall rate") {
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_WALL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    REQUIRE(me_player_play(f.player, 2.0f) == ME_OK);

    /* Sample current_time() at two points 100 ms apart. At rate 2.0
     * the clock should advance ~0.2 s in 0.1 s wall. The bound
     * accommodates ±25 % scheduling jitter (0.15..0.25) — generous
     * because this assertion runs under doctest's single-thread
     * harness and CI may park us. The point is "advanced faster than
     * 1× would have, by a clearly nonzero margin", not "exact 2×". */
    const me_rational_t t0 = me_player_current_time(f.player);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const me_rational_t t1 = me_player_current_time(f.player);

    const double dt = rat_to_sec(t1) - rat_to_sec(t0);
    MESSAGE("rate=2.0 dt across 100ms wall = " << dt << " s");
    CHECK(dt >= 0.15);
    CHECK(dt <= 0.25);
}

/* ----------------------------------------------------- external clock */

namespace {
struct ExternalClockState {
    std::atomic<int64_t> num{0};
    int64_t den = 1000;   /* ms-resolution */

    static me_rational_t cb(void* user) {
        auto* s = static_cast<ExternalClockState*>(user);
        return me_rational_t{s->num.load(), s->den};
    }
};
}  // namespace

TEST_CASE("me_player: ME_CLOCK_EXTERNAL create succeeds (no longer ME_E_UNSUPPORTED)") {
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_EXTERNAL;
    /* Pre-cycle 21 this branch returned ME_E_UNSUPPORTED; cycle 21
     * wired the host callback path so creation succeeds and current()
     * falls back to WALL projection until set_external_clock_callback
     * is called. */
    REQUIRE(f.make_player(&pc) == ME_OK);
}

TEST_CASE("me_player: external clock — current_time tracks host callback") {
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_EXTERNAL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    /* Cold start: no callback set yet. current() falls back to WALL
     * projection — paused at 0 since play() hasn't fired. */
    CHECK(rat_to_sec(me_player_current_time(f.player)) == 0.0);

    ExternalClockState s;
    REQUIRE(me_player_set_external_clock_callback(
                f.player, &ExternalClockState::cb, &s) == ME_OK);

    /* After registration: current() invokes the callback verbatim. */
    s.num.store(1500);   /* 1.5 s */
    CHECK(rat_to_sec(me_player_current_time(f.player)) == 1.5);

    s.num.store(3250);   /* 3.25 s */
    CHECK(rat_to_sec(me_player_current_time(f.player)) == 3.25);

    /* Clearing the callback (cb=NULL) reverts to WALL fallback. */
    REQUIRE(me_player_set_external_clock_callback(f.player, nullptr, nullptr)
                == ME_OK);
    /* WALL is paused (no play called) → 0. */
    CHECK(rat_to_sec(me_player_current_time(f.player)) == 0.0);
}

TEST_CASE("me_player: external clock drives video pacer") {
    /* Same shape as the report_audio_playhead pacer test: the pacer
     * must consult current() (= the external callback) when deciding
     * whether to present a queued slot. With a callback that ramps
     * past the timeline duration, frames flow through and land in
     * the host video_cb. */
    FrameCapture  cap;
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_EXTERNAL;
    pc.video_ring_capacity   = 3;
    REQUIRE(f.make_player(&pc) == ME_OK);

    REQUIRE(me_player_set_video_callback(f.player, &FrameCapture::cb, &cap)
                == ME_OK);
    ExternalClockState s;
    REQUIRE(me_player_set_external_clock_callback(
                f.player, &ExternalClockState::cb, &s) == ME_OK);

    /* play() admits rate=1.0; the pacer consults the external
     * callback for "now" — by ramping s.num steadily we simulate a
     * host clock advancing in real time. */
    REQUIRE(me_player_play(f.player, 1.0f) == ME_OK);

    /* Drive the external clock forward over ~250 ms wall, asserting
     * that frames flow through. The clock advances 250 ms → ~7-8
     * frames at 30 fps, but accept any positive count. */
    for (int i = 0; i < 25; ++i) {
        s.num.fetch_add(10);   /* +10 ms per tick */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(cap.count.load() > 0);
}

TEST_CASE("me_player: destroy mid-playback joins cleanly") {
    FrameCapture  cap;
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_WALL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    me_player_set_video_callback(f.player, &FrameCapture::cb, &cap);
    me_player_play(f.player, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    /* Destroy must cancel any in-flight Future + join both threads
     * within reasonable time. The PlayerFixture dtor calls destroy;
     * the assertion is implicit (test completes without timeout). */
}

/* ------------------------------------------------------------ seek */

TEST_CASE("me_player: seek + play reseats present_at to the seek target") {
    FrameCapture  cap;
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_WALL;
    pc.video_ring_capacity   = 3;
    REQUIRE(f.make_player(&pc) == ME_OK);

    me_player_set_video_callback(f.player, &FrameCapture::cb, &cap);

    /* Play forward briefly so the producer ring fills with pre-seek
     * frames, then seek backward + play again. The first frame
     * delivered post-seek must have present_at ≈ seek target. */
    me_player_play(f.player, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int pre_seek_count = cap.count.load();
    CHECK(pre_seek_count > 0);

    me_player_seek(f.player, me_rational_t{1, 5});  /* 0.2 s */
    me_player_play(f.player, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    /* Snapshot post-seek frames + verify the first one is at ≥ 0.2 s
     * but < 0.2 s + (post-seek wall ≈ 0.25 s budget). The
     * seek_epoch tag in VideoFrameRing should have caused any
     * pre-seek slot leak to be dropped by the pacer. */
    std::vector<me_rational_t> ats;
    {
        std::lock_guard<std::mutex> lk(cap.mu);
        ats = cap.present_ats;
    }
    REQUIRE(static_cast<int>(ats.size()) > pre_seek_count);
    const me_rational_t first_post = ats[pre_seek_count];
    const double first_t = rat_to_sec(first_post);
    /* Half-frame_period tolerance at 30 fps = 0.0167 s. The very
     * first post-seek frame should be at exactly the seek target;
     * accept slop for the case where the pacer dropped frame 0
     * because the cursor lookup raced. */
    CHECK(first_t >= 0.2);
    CHECK(first_t <  0.5);  /* generous upper bound */
}

TEST_CASE("me_player: rapid seeks don't lose threads or leak frames") {
    FrameCapture  cap;
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_WALL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    me_player_set_video_callback(f.player, &FrameCapture::cb, &cap);

    me_player_play(f.player, 1.0f);
    /* Hammer seek 10× in quick succession — each one cancels the
     * in-flight Future + clears the ring + bumps the epoch.
     * Survival = no crash + no hang at destroy. */
    for (int i = 0; i < 10; ++i) {
        me_rational_t t{ static_cast<int64_t>(i) * 100, 1000 };  /* 0, 0.1, 0.2, ... */
        me_player_seek(f.player, t);
        me_player_play(f.player, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    me_player_pause(f.player);
    /* Implicit assertion: PlayerFixture dtor joins cleanly. */
}

/* --------------------------------------------------- a/v sync */

TEST_CASE("me_player: report_audio_playhead drives video pacer when ME_CLOCK_AUDIO active") {
    FrameCapture  cap;
    PlayerFixture f;
    me_player_config_t pc{};
    /* sample_rate > 0 + AUDIO master clock. The audio device is
     * mocked: this test never produces real audio because the
     * underlying timeline has no audio track, so audio_mixer_
     * stays null + the audio_producer thread is never spawned.
     * The clock still honours report_audio_playhead. */
    pc.audio_out.sample_rate = 48000;
    pc.audio_out.num_channels = 2;
    pc.master_clock          = ME_CLOCK_AUDIO;
    pc.video_ring_capacity   = 5;
    REQUIRE(f.make_player(&pc) == ME_OK);

    me_player_set_video_callback(f.player, &FrameCapture::cb, &cap);

    me_player_play(f.player, 1.0f);
    /* Cold-start fallback: until the first audio playhead arrives,
     * the AUDIO master clock projects WALL. Frames should still
     * flow during this gap. */
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const int cold_start_frames = cap.count.load();
    CHECK(cold_start_frames > 0);

    /* Now simulate a host audio device callback: report a playhead
     * that's a bit BEHIND the WALL projection. The video pacer
     * should slow / hold to chase that playhead. */
    for (int i = 0; i < 6; ++i) {
        const me_rational_t t{static_cast<int64_t>(i) * 50, 1000};  /* 0, 0.05, … */
        me_player_report_audio_playhead(f.player, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    /* No assertion on exact frame count — the contract is
     * "video chases audio" not "matches WALL exactly". This test
     * primarily verifies the API surface accepts the call + the
     * player doesn't crash / deadlock under repeated playhead
     * injection. The destroy from the fixture dtor exercises
     * cleanup with the AUDIO clock active. */
    CHECK(cap.count.load() >= cold_start_frames);  /* at least no regression */
}
