/*
 * 07_compose_multitrack — 2-track compose render.
 *
 * Builds a 2-track timeline on the fly where both tracks point at
 * the same source file. Loader opens independent decoders per
 * clip so the compose loop sees two real decoded streams; the
 * top track alpha-overs the bottom at opacity 0.5.
 *
 * Purpose: show hosts how to drive the multi-track compose path
 * via only the public C API — the test suite covers correctness,
 * this is the "minimum viable example".
 *
 * Usage:
 *   07_compose_multitrack <source.mp4> <output.mp4>
 *
 * Source must have a video stream. Output is always MP4 /
 * h264_videotoolbox + AAC (compose requires reencode — passthrough
 * isn't supported on the compose path).
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_progress(const me_progress_event_t* ev, void* user) {
    (void)user;
    switch (ev->kind) {
    case ME_PROGRESS_STARTED:   fprintf(stderr, "  started\n"); break;
    case ME_PROGRESS_FRAMES:    fprintf(stderr, "\r  %5.1f%%", ev->ratio * 100.0f); fflush(stderr); break;
    case ME_PROGRESS_COMPLETED: fprintf(stderr, "\n  done → %s\n", ev->output_path); break;
    case ME_PROGRESS_FAILED:    fprintf(stderr, "\n  failed: %s\n", me_status_str(ev->status)); break;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <source.mp4> <output.mp4>\n", argv[0]);
        return 2;
    }
    const char* source = argv[1];
    const char* output = argv[2];

    /* Build the 2-track JSON inline. Both tracks use the same
     * source asset; `v0` (bottom, opaque) has no transform, `v1`
     * (top) alpha-overs at opacity 0.5 — same shape as
     * test_compose_sink_e2e's "2-track with per-clip transform
     * opacity" case. 1 s duration @ 25 fps. */
    char uri[1024];
    snprintf(uri, sizeof uri, "file://%s", source);

    char json[4096];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":25,\"den\":1},\n"
        "  \"resolution\": {\"width\":640,\"height\":480},\n"
        "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a1\",\"kind\":\"video\",\"uri\":\"%s\"}],\n"
        "  \"compositions\": [{\"id\":\"main\",\"tracks\":[\n"
        "    {\"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "      {\"type\":\"video\",\"id\":\"c_v0\",\"assetId\":\"a1\",\n"
        "       \"timeRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}},\n"
        "       \"sourceRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}}}\n"
        "    ]},\n"
        "    {\"id\":\"v1\",\"kind\":\"video\",\"clips\":[\n"
        "      {\"type\":\"video\",\"id\":\"c_v1\",\"assetId\":\"a1\",\n"
        "       \"transform\":{\"opacity\":{\"static\":0.5}},\n"
        "       \"timeRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}},\n"
        "       \"sourceRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}}}\n"
        "    ]}\n"
        "  ]}],\n"
        "  \"output\": {\"compositionId\":\"main\"}\n"
        "}\n", uri);

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(NULL, &eng);
    if (s != ME_OK) { fprintf(stderr, "engine_create: %s\n", me_status_str(s)); return 1; }

    me_timeline_t* tl = NULL;
    s = me_timeline_load_json(eng, json, strlen(json), &tl);
    if (s != ME_OK) {
        fprintf(stderr, "load_json: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    me_output_spec_t spec = {0};
    spec.path        = output;
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    me_render_job_t* job = NULL;
    s = me_render_start(eng, tl, &spec, on_progress, NULL, &job);
    if (s != ME_OK) {
        fprintf(stderr, "render_start: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_timeline_destroy(tl);
        me_engine_destroy(eng);
        return 1;
    }
    s = me_render_wait(job);
    me_render_job_destroy(job);
    me_timeline_destroy(tl);

    if (s != ME_OK) {
        fprintf(stderr, "render: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    me_engine_destroy(eng);
    return 0;
}
