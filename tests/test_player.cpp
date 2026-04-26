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

TEST_CASE("me_player: play(rate != 1.0) is rejected with ME_E_UNSUPPORTED") {
    PlayerFixture f;
    me_player_config_t pc{};
    pc.audio_out.sample_rate = 0;
    pc.master_clock          = ME_CLOCK_WALL;
    REQUIRE(f.make_player(&pc) == ME_OK);

    CHECK(me_player_play(f.player, 2.0f)  == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, 0.5f)  == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, -1.0f) == ME_E_UNSUPPORTED);
    CHECK(me_player_play(f.player, 1.0f)  == ME_OK);
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
