/*
 * 15_body_alpha_key — green-screen-without-greenscreen via portrait
 * segmentation alpha mask.
 *
 * Builds a timeline with a single video clip + a `body_alpha_key`
 * effect, runs it through `me_render_start` to produce an MP4
 * whose alpha channel is the input alpha multiplied by the
 * resolved per-frame mask.
 *
 * The body_alpha_key stage's mask resolver runs in file mode (M11
 * cycle d8389e4): the host passes a JSON sidecar with a `frames`
 * array, each entry carrying a base64-encoded 8-bit alpha plane
 * (one byte per pixel, row-major, exactly width*height bytes).
 * The runtime-driven variant (calling
 * `me::inference::Runtime::run` for portrait segmentation, e.g.
 * SelfieSegmentation) plugs into the same compose stage via
 * `me::inference::run_cached` — that's a separate axis (cycle
 * 653521e + later).
 *
 * Mask dimensions MUST match the timeline's frame dimensions.
 * The companion sample.mask.json is sized 16×12; the timeline
 * here matches with `width=16, height=12`. To use a different
 * resolution, generate a new mask sized to the timeline you set.
 *
 * Usage:
 *   15_body_alpha_key <source.mp4> <mask.json> <output.mp4>
 *
 * - source.mp4 is the input video (any resolution; the engine
 *   rescales to the timeline's resolution).
 * - mask.json is the per-frame alpha plane sequence; shape:
 *
 *     {
 *       "frames": [
 *         { "t": {"num": 0, "den": 30},
 *           "width": 16, "height": 12,
 *           "alphaB64": "<base64 of 16*12 raw bytes>" },
 *         ...
 *       ]
 *     }
 *
 * - output.mp4 is the rendered file. Bottom half of each frame
 *   should preserve the input's alpha (mask = 255); top half
 *   should be alpha-keyed out (mask = 0) — the committed
 *   sample.mask.json encodes that pattern.
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
                "usage: %s <source.mp4> <mask.json> <output.mp4>\n",
                argv[0]);
        return 2;
    }
    const char* source = argv[1];
    const char* mask   = argv[2];
    const char* output = argv[3];

    char source_uri[1024];
    snprintf(source_uri, sizeof source_uri, "file://%s", source);

    /* 1-second timeline at 30 fps, resolution matched to the
     * committed sample.mask.json (16×12). The mask asset's URI
     * is a path the resolver opens directly (mask_resolver
     * accepts both file:// and absolute-path forms). */
    char json[4096];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\":1,\n"
        "  \"frameRate\":{\"num\":30,\"den\":1},\n"
        "  \"resolution\":{\"width\":16,\"height\":12},\n"
        "  \"colorSpace\":{\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\":[\n"
        "    {\"id\":\"v1\",\"kind\":\"video\",\"uri\":\"%s\"},\n"
        "    {\"id\":\"mask1\",\"uri\":\"%s\",\"type\":\"mask\",\n"
        "     \"model\":{\"id\":\"selfie_seg\",\"version\":\"v3\",\"quantization\":\"int8\"}}\n"
        "  ],\n"
        "  \"compositions\":[{\"id\":\"main\",\"tracks\":[{\"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "    {\"type\":\"video\",\"id\":\"c1\",\"assetId\":\"v1\",\n"
        "     \"effects\":[{\"kind\":\"body_alpha_key\",\n"
        "                  \"params\":{\"maskAssetId\":\"mask1\",\n"
        "                              \"featherRadiusPx\":0,\n"
        "                              \"invert\":false}}],\n"
        "     \"timeRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":30,\"den\":30}},\n"
        "     \"sourceRange\":{\"start\":{\"num\":0,\"den\":30},\"duration\":{\"num\":30,\"den\":30}}}\n"
        "  ]}]}],\n"
        "  \"output\":{\"compositionId\":\"main\"}\n"
        "}\n",
        source_uri, mask);

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
