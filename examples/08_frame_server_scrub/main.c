/*
 * 08_frame_server_scrub — scrubbing via me_render_frame.
 *
 * Builds a 1-clip timeline over `<source.mp4>`, pulls RGBA frames
 * at 5 distinct times (0.0, 0.25, 0.5, 0.75, 1.0 × clip duration),
 * writes each as a PPM under `<out-dir>`, and prints
 * me_cache_stats before / after to show hit/miss evolution.
 *
 * Purpose: demo the M6 frame-server path hosts would use to
 * drive a scrubbing UI (thumbnail strip / preview panel). Uses
 * only the public C API.
 *
 * Usage:
 *   08_frame_server_scrub <source.mp4> <out-dir>
 *
 * Writes `<out-dir>/frame_0.ppm` ... `frame_4.ppm`.
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int write_ppm(const char* path, const me_frame_t* frame) {
    const int w = me_frame_width(frame);
    const int h = me_frame_height(frame);
    const uint8_t* px = me_frame_pixels(frame);
    if (!px || w <= 0 || h <= 0) return -1;

    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* me_frame pixels are RGBA8 row-major, stride = w × 4. PPM is
     * RGB; skip the alpha channel. */
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t rgb[3] = { row[x*4+0], row[x*4+1], row[x*4+2] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

static void print_stats(const char* label, const me_cache_stats_t* s) {
    fprintf(stderr, "  [%s] mem_used=%lld mem_limit=%lld disk_used=%lld "
                    "hits=%lld misses=%lld entries=%lld codecs=%lld\n",
            label,
            (long long)s->memory_bytes_used,  (long long)s->memory_bytes_limit,
            (long long)s->disk_bytes_used,
            (long long)s->hit_count,          (long long)s->miss_count,
            (long long)s->entry_count,        (long long)s->codec_ctx_count);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <source.mp4> <out-dir>\n", argv[0]);
        return 2;
    }
    const char* source  = argv[1];
    const char* out_dir = argv[2];

    /* Best-effort mkdir — skip the errno check because the output
     * loop will surface a clearer message if the dir truly can't
     * be written. */
    mkdir(out_dir, 0755);

    /* Enable a disk cache so the scrub-back path exercises
     * DiskCache::get (hit the second time we fetch the same
     * time). */
    me_engine_config_t cfg = {0};
    const char* cache_dir = "/tmp/me_frame_server_scrub_cache";
    cfg.cache_dir = cache_dir;

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(&cfg, &eng);
    if (s != ME_OK) { fprintf(stderr, "engine_create: %s\n", me_status_str(s)); return 1; }

    char uri[1024];
    snprintf(uri, sizeof uri, "file://%s", source);

    /* 2-second single-clip video-only timeline; 5 samples at
     * 0, 0.5, 1.0, 1.5, 2.0 seconds cover the range. */
    char json[2048];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":25,\"den\":1},\n"
        "  \"resolution\": {\"width\":640,\"height\":480},\n"
        "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a1\",\"uri\":\"%s\"}],\n"
        "  \"compositions\": [{\n"
        "    \"id\":\"main\",\n"
        "    \"duration\":{\"num\":2,\"den\":1},\n"
        "    \"tracks\":[{\n"
        "      \"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "        {\"type\":\"video\",\"id\":\"c0\",\"assetId\":\"a1\",\n"
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

    me_cache_stats_t before = {0};
    me_cache_stats(eng, &before);
    print_stats("before", &before);

    /* 4 scrub points at 0, 0.5, 1.0, 1.5 s — strictly inside the
     * 2-second clip's half-open [0, 2) range. t=2.0 would fall
     * past the active-clip test and return ME_E_NOT_FOUND. */
    for (int i = 0; i < 4; ++i) {
        const me_rational_t t = { (int64_t)i, 2 };  /* 0, 1/2, 2/2, 3/2 */
        me_frame_t* frame = NULL;
        s = me_render_frame(eng, tl, t, &frame);
        if (s != ME_OK) {
            fprintf(stderr, "render_frame[%d]: %s (%s)\n",
                    i, me_status_str(s), me_engine_last_error(eng));
            continue;
        }
        char out_path[1024];
        snprintf(out_path, sizeof out_path, "%s/frame_%d.ppm", out_dir, i);
        if (write_ppm(out_path, frame) == 0) {
            fprintf(stderr, "  wrote %s (%dx%d)\n",
                    out_path, me_frame_width(frame), me_frame_height(frame));
        }
        me_frame_destroy(frame);
    }

    me_cache_stats_t after = {0};
    me_cache_stats(eng, &after);
    print_stats("after ", &after);

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return 0;
}
