/*
 * 01_passthrough — stream-copy a single-clip timeline to MP4.
 *
 * Purpose: end-to-end exercise of me_engine_create → me_timeline_load_json →
 * me_render_start → me_render_wait, using only the public C API.
 *
 * Usage:
 *   01_passthrough <timeline.json> <output.mp4>
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_progress(const me_progress_event_t* ev, void* user) {
    (void)user;
    switch (ev->kind) {
    case ME_PROGRESS_STARTED:
        fprintf(stderr, "  started\n");
        break;
    case ME_PROGRESS_FRAMES:
        fprintf(stderr, "\r  %5.1f%%", ev->ratio * 100.0f);
        fflush(stderr);
        break;
    case ME_PROGRESS_COMPLETED:
        fprintf(stderr, "\n  done → %s\n", ev->output_path);
        break;
    case ME_PROGRESS_FAILED:
        fprintf(stderr, "\n  failed: %s\n", me_status_str(ev->status));
        break;
    }
}

static char* slurp(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = got;
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <timeline.json> <output.mp4>\n", argv[0]);
        return 2;
    }

    me_version_t v = me_version();
    fprintf(stderr, "media-engine %d.%d.%d\n", v.major, v.minor, v.patch);

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(NULL, &eng);
    if (s != ME_OK) { fprintf(stderr, "engine_create: %s\n", me_status_str(s)); return 1; }

    size_t json_len = 0;
    char* json = slurp(argv[1], &json_len);
    if (!json) { fprintf(stderr, "cannot read %s\n", argv[1]); me_engine_destroy(eng); return 1; }

    me_timeline_t* tl = NULL;
    s = me_timeline_load_json(eng, json, json_len, &tl);
    free(json);
    if (s != ME_OK) {
        fprintf(stderr, "load_json: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    me_output_spec_t out = {0};
    out.path        = argv[2];
    out.container   = "mp4";
    out.video_codec = "passthrough";
    out.audio_codec = "passthrough";

    me_render_job_t* job = NULL;
    s = me_render_start(eng, tl, &out, on_progress, NULL, &job);
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
