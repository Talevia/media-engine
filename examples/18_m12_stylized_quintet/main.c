/*
 * 18_m12_stylized_quintet — JSON loader smoke test for the
 * five M12 stylized effects (glitch, scan_lines,
 * chromatic_aberration, posterize, ordered_dither).
 *
 * See examples/17_m12_color_quartet/main.c for the rationale
 * — this example mirrors that one's shape: load JSON,
 * verify all five kinds parse, print success, exit. Per-
 * kernel pixel correctness lives in the dedicated test
 * suites.
 *
 * Usage:
 *   18_m12_stylized_quintet [timeline.json]
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char* path = (argc >= 2) ? argv[1] : "sample.timeline.json";
    fprintf(stderr, "18_m12_stylized_quintet — loading %s\n", path);

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(NULL, &eng);
    if (s != ME_OK) {
        fprintf(stderr, "engine_create: %s\n", me_status_str(s));
        return 1;
    }

    size_t json_len = 0;
    char* json = slurp(path, &json_len);
    if (!json) {
        fprintf(stderr, "cannot read %s\n", path);
        me_engine_destroy(eng);
        return 1;
    }

    me_timeline_t* tl = NULL;
    s = me_timeline_load_json(eng, json, json_len, &tl);
    free(json);
    if (s != ME_OK) {
        fprintf(stderr, "load_json: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    fprintf(stderr,
            "  loaded — glitch + scan_lines + chromatic_aberration + "
            "posterize + ordered_dither all parsed successfully\n");

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return 0;
}
