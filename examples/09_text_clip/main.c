/*
 * 09_text_clip — video + animated-text overlay via compose.
 *
 * Builds a timeline with a video bottom track + a text top track
 * carrying keyframed color + keyframed fontSize + maxWidth
 * paragraph wrap + explicit `\n` multi-line. Renders via
 * me_render_start to an MP4 with text composited on top of
 * source.
 *
 * Purpose: demo the M5 text-clip + animated-field + paragraph-
 * wrap path hosts would use for lower-third captions / overlay
 * text. All M5 features exercised via only the public C API.
 *
 * Why not me_render_frame? — Previewer's phase-1 frame server
 * walks the bottom track's active clip only (see
 * src/orchestrator/previewer.cpp's clip lookup). Multi-track
 * composite-through-preview is a deferred feature. So text-clip
 * rendering today goes through the compose sink, which runs via
 * me_render_start with video_codec=h264, audio_codec=aac.
 *
 * Usage:
 *   09_text_clip <source.mp4> <output.mp4>
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

    char uri[1024];
    snprintf(uri, sizeof uri, "file://%s", source);

    /* 1-second timeline: video bottom track + text top track.
     *   - color: yellow-semi-transparent → white-opaque (animated).
     *   - fontSize: 32 → 48 (animated).
     *   - maxWidth: 400 px, lineHeightMultiplier 1.3 → wraps
     *     content, explicit \n produces an extra line break. */
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
        "      {\"type\":\"video\",\"id\":\"c_v\",\"assetId\":\"a1\",\n"
        "       \"timeRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}},\n"
        "       \"sourceRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}}}\n"
        "    ]},\n"
        "    {\"id\":\"t0\",\"kind\":\"text\",\"clips\":[\n"
        "      {\"type\":\"text\",\"id\":\"c_t\",\n"
        "       \"timeRange\":{\"start\":{\"num\":0,\"den\":25},\"duration\":{\"num\":25,\"den\":25}},\n"
        "       \"textParams\":{\n"
        "         \"content\":\"你好 world\\nsecond line\",\n"
        "         \"color\":{\"keyframes\":[\n"
        "           {\"t\":{\"num\":0,\"den\":1}, \"v\":\"#FFFF0080\", \"interp\":\"linear\"},\n"
        "           {\"t\":{\"num\":1,\"den\":1}, \"v\":\"#FFFFFFFF\", \"interp\":\"linear\"}\n"
        "         ]},\n"
        "         \"fontSize\":{\"keyframes\":[\n"
        "           {\"t\":{\"num\":0,\"den\":1}, \"v\":32, \"interp\":\"linear\"},\n"
        "           {\"t\":{\"num\":1,\"den\":1}, \"v\":48, \"interp\":\"linear\"}\n"
        "         ]},\n"
        "         \"x\":{\"static\":20},\n"
        "         \"y\":{\"static\":80},\n"
        "         \"maxWidth\":400,\n"
        "         \"lineHeightMultiplier\":1.3\n"
        "       }}\n"
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
