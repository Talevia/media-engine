/*
 * 16_face_mosaic — face-region pixelation / blur via landmark
 * bboxes (privacy mosaic).
 *
 * Builds a timeline with a single video clip + a `face_mosaic`
 * effect, runs it through `me_render_start` to produce an MP4
 * with each landmark-resolved bbox replaced by a per-block mean
 * (Pixelate) or box-filtered region (Blur).
 *
 * The face_mosaic stage's landmark resolver runs in file mode
 * (M11 cycle 1d7a4b1): the host passes a JSON sidecar with a
 * `frames` array, each entry carrying the bboxes for a specific
 * frame timestamp. The runtime-driven variant (calling
 * `me::inference::Runtime::run` for face detection, e.g.
 * BlazeFace) plugs into the same compose stage via
 * `me::inference::run_cached` and is a separate axis (cycle
 * 653521e + later).
 *
 * Sibling of 13_face_sticker — same compose-stage chain shape
 * (Demux → Decode → Convert → FaceMosaic → output) and same
 * file-mode landmark resolver. Differs in the effect: mosaic
 * doesn't take a sticker PNG, so the CLI is one arg shorter
 * (3 args: source.mp4 + landmarks.json + output.mp4).
 *
 * Usage:
 *   16_face_mosaic <source.mp4> <landmarks.json> <output.mp4>
 *
 * - source.mp4 is the input video.
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
 * The companion sample.landmarks.json is committed; hosts only
 * need to supply source.mp4. The example uses kind="pixelate"
 * with block_size_px=16 — change to "blur" inline if you'd
 * prefer the box-filter look.
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
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <source.mp4> <landmarks.json> <output.mp4>\n",
                argv[0]);
        return 2;
    }
    const char* source    = argv[1];
    const char* landmarks = argv[2];
    const char* output    = argv[3];

    char source_uri[1024];
    snprintf(source_uri, sizeof source_uri, "file://%s", source);

    /* 2-second timeline at 30 fps, 640x480. Single video clip
     * with one face_mosaic effect referencing the landmark
     * asset. Default block_size_px=16 + kind="pixelate" gives
     * a noticeable mosaic look on face-sized bboxes (~12 blocks
     * across a 200-px-wide face). */
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
        "     \"effects\":[{\"kind\":\"face_mosaic\",\n"
        "                  \"params\":{\"landmarkAssetId\":\"ml1\",\n"
        "                              \"blockSizePx\":16,\n"
        "                              \"kind\":\"pixelate\"}}],\n"
        "     \"timeRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":60,\"den\":30}},\n"
        "     \"sourceRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":60,\"den\":30}}}\n"
        "  ]}]}],\n"
        "  \"output\":{\"compositionId\":\"main\"}\n"
        "}\n",
        source_uri, landmarks);

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
