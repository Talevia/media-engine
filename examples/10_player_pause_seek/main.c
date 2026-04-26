/*
 * 10_player_pause_seek — end-to-end demo of me_player_*: play, pause,
 * seek, callback-driven frame delivery.
 *
 * Drives the player over a 1-clip video timeline of `<source.mp4>`
 * for ~3 seconds, exercising the full transport surface. Logs each
 * delivered frame's `present_at` timestamp to stdout so a reader can
 * see frames flowing in monotonic time during play, frozen during
 * pause, and jumping back to the seek target after seek.
 *
 * Audio is intentionally disabled (`audio_out.sample_rate == 0`) so
 * the example runs anywhere — no audio device required, master clock
 * forced to WALL.
 *
 * Usage:
 *   10_player_pause_seek <source.mp4>
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Counters shared with the video callback. */
static int g_frame_count = 0;

static void on_video_frame(const me_frame_t* frame,
                            me_rational_t      present_at,
                            void*              user) {
    (void)user;
    if (!frame) return;
    const int w = me_frame_width(frame);
    const int h = me_frame_height(frame);
    /* Lossy float just for human-readable display — the stable
     * timestamp is in (num,den). */
    const double t_sec = present_at.den != 0
        ? (double)present_at.num / (double)present_at.den
        : 0.0;
    ++g_frame_count;
    fprintf(stderr, "  frame #%d  t=%6.3fs  %dx%d  (%lld/%lld)\n",
            g_frame_count, t_sec, w, h,
            (long long)present_at.num, (long long)present_at.den);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <source.mp4>\n", argv[0]);
        return 2;
    }
    const char* source = argv[1];

    me_engine_t* eng = NULL;
    me_engine_config_t engine_cfg = {0};
    me_status_t s = me_engine_create(&engine_cfg, &eng);
    if (s != ME_OK) {
        fprintf(stderr, "engine_create: %s\n", me_status_str(s));
        return 1;
    }

    /* Build a 2-second video-only timeline over the source. Same
     * shape as 08_frame_server_scrub for symmetry. */
    char uri[1024];
    snprintf(uri, sizeof uri, "file://%s", source);

    char json[2048];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":30,\"den\":1},\n"
        "  \"resolution\": {\"width\":160,\"height\":120},\n"
        "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a0\",\"uri\":\"%s\"}],\n"
        "  \"compositions\": [{\n"
        "    \"id\":\"main\",\n"
        "    \"duration\":{\"num\":2,\"den\":1},\n"
        "    \"tracks\":[{\n"
        "      \"id\":\"t0\",\"kind\":\"video\",\"clips\":[\n"
        "        {\"id\":\"c0\",\"type\":\"video\",\"assetId\":\"a0\",\n"
        "         \"timeRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}},\n"
        "         \"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}}}\n"
        "      ]}]\n"
        "  }],\n"
        "  \"output\": {\"compositionId\":\"main\"}\n"
        "}\n", uri);

    me_timeline_t* tl = NULL;
    s = me_timeline_load_json(eng, json, strlen(json), &tl);
    if (s != ME_OK) {
        fprintf(stderr, "load_json: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    me_player_config_t cfg = {0};
    /* sample_rate=0 disables audio + forces ME_CLOCK_WALL. The
     * example doesn't need a real audio device. */
    cfg.audio_out.sample_rate = 0;
    cfg.master_clock          = ME_CLOCK_WALL;
    cfg.video_ring_capacity   = 3;
    cfg.audio_queue_ms        = 0;

    me_player_t* player = NULL;
    s = me_player_create(eng, tl, &cfg, &player);
    if (s != ME_OK) {
        fprintf(stderr, "player_create: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_timeline_destroy(tl);
        me_engine_destroy(eng);
        return 1;
    }
    me_player_set_video_callback(player, on_video_frame, NULL);

    /* Transport sequence:
     *   t=0     play()
     *   t=800   pause()       (expect ~24 frames already; counter freezes)
     *   t=1100  play()        (resume from where we paused, ≈ 0.8 s)
     *   t=1700  seek(0.4s)    (cancel any in-flight + reseat)
     *   t=2400  pause()       (terminate cleanly)
     *
     * The exact frame count varies with decoder warm-up and host
     * scheduling; this example optimises for being a readable demo,
     * not a deterministic test. */
    fprintf(stderr, "[0.0s] play(1.0)\n");
    me_player_play(player, 1.0f);
    sleep_ms(800);

    fprintf(stderr, "[0.8s] pause() — frame count so far: %d\n", g_frame_count);
    me_player_pause(player);
    const int frames_at_pause = g_frame_count;
    sleep_ms(300);
    if (g_frame_count != frames_at_pause) {
        fprintf(stderr, "  WARN: frames delivered while paused: %d → %d\n",
                frames_at_pause, g_frame_count);
    } else {
        fprintf(stderr, "  pause held — no frames delivered for 300ms\n");
    }

    fprintf(stderr, "[1.1s] play(1.0)\n");
    me_player_play(player, 1.0f);
    sleep_ms(600);

    fprintf(stderr, "[1.7s] seek(0.4s) + play(1.0)\n");
    const int frames_pre_seek = g_frame_count;
    me_player_seek(player, (me_rational_t){2, 5});  /* 0.4s = 2/5 */
    /* seek() snaps to the target and stays paused — caller resumes
     * explicitly. This is the API contract: seek = "move playhead",
     * play = "transport on". */
    me_player_play(player, 1.0f);
    sleep_ms(1200);
    fprintf(stderr, "  post-seek frames delivered: %d (expect timestamps re-anchored at ≥ 0.4s)\n",
            g_frame_count - frames_pre_seek);

    fprintf(stderr, "[2.4s] pause + tear down — total frames: %d\n",
            g_frame_count);
    me_player_pause(player);
    me_player_destroy(player);

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return 0;
}
