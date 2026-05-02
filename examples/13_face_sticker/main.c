/*
 * 13_face_sticker — overlay a sticker on detected faces.
 *
 * Builds a timeline with a single video clip + a `face_sticker`
 * effect, runs it through `me_render_start` to produce an MP4
 * with the sticker source-over-blended onto landmark bboxes per
 * frame.
 *
 * The face_sticker stage's landmark resolver runs in file mode
 * (M11 cycle 4a89dae): the host passes a JSON sidecar with a
 * `frames` array, each entry carrying the bboxes for a specific
 * frame timestamp. The runtime-driven variant (calling
 * `me::inference::Runtime::run` for face detection, e.g.
 * BlazeFace) plugs into the same compose stage via
 * `me::inference::run_cached` and is a separate axis (cycle
 * 653521e + later).
 *
 * Usage:
 *   13_face_sticker <source.mp4> <sticker.png> <landmarks.json> <output.mp4>
 *
 * - source.mp4 is the input video.
 * - sticker.png is the RGBA8 PNG to overlay.
 * - landmarks.json is the bbox-per-frame fixture; shape:
 *
 *     {
 *       "frames": [
 *         { "t": {"num": 0,  "den": 30},
 *           "bboxes": [ {"x0":..., "y0":..., "x1":..., "y1":...} ] },
 *         ...
 *       ]
 *     }
 *
 * - output.mp4 is the rendered file.
 *
 * For a quick smoke run without producing a sticker fixture, the
 * companion sample.timeline.json + tiny sample.landmarks.json in
 * this directory let you skip steps 1-3 and jump straight to step
 * 4 with `cmake --build` artifacts (sticker fixture is left to
 * the host since PNG payloads aren't binary-friendly to commit).
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
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <source.mp4> <sticker.png> <landmarks.json> <output.mp4>\n",
                argv[0]);
        return 2;
    }
    const char* source     = argv[1];
    const char* sticker    = argv[2];
    const char* landmarks  = argv[3];
    const char* output     = argv[4];

    char source_uri[1024], sticker_uri[1024];
    snprintf(source_uri,  sizeof source_uri,  "file://%s", source);
    snprintf(sticker_uri, sizeof sticker_uri, "file://%s", sticker);

    /* 2-second timeline at 30 fps. Single video clip with one
     * face_sticker effect referencing the landmark asset. The
     * landmark asset's URI is a path the resolver opens directly
     * (mask_resolver / landmark_resolver accept both file:// and
     * absolute-path forms). */
    char json[4096];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\":1,\n"
        "  \"frameRate\":{\"num\":30,\"den\":1},\n"
        "  \"resolution\":{\"width\":640,\"height\":480},\n"
        "  \"colorSpace\":{\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\":[\n"
        "    {\"id\":\"v1\",\"kind\":\"video\",\"uri\":\"%s\"},\n"
        "    {\"id\":\"ml1\",\"uri\":\"%s\",\"type\":\"landmark\",\n"
        "     \"model\":{\"id\":\"blazeface\",\"version\":\"v2\",\"quantization\":\"fp16\"}}\n"
        "  ],\n"
        "  \"compositions\":[{\"id\":\"main\",\"tracks\":[{\"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "    {\"type\":\"video\",\"id\":\"c1\",\"assetId\":\"v1\",\n"
        "     \"effects\":[{\"kind\":\"face_sticker\",\n"
        "                  \"params\":{\"landmarkAssetId\":\"ml1\",\n"
        "                              \"stickerUri\":\"%s\",\n"
        "                              \"scaleX\":1.0,\"scaleY\":1.0,\n"
        "                              \"offsetX\":0.0,\"offsetY\":0.0}}],\n"
        "     \"timeRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":60,\"den\":30}},\n"
        "     \"sourceRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":60,\"den\":30}}}\n"
        "  ]}]}],\n"
        "  \"output\":{\"compositionId\":\"main\"}\n"
        "}\n",
        source_uri, landmarks, sticker_uri);

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

    /* h264/aac re-encode path so the face_sticker compose stage
     * runs (passthrough wouldn't apply effects). */
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
