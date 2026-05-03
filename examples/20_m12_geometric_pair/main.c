/*
 * 20_m12_geometric_pair — JSON loader smoke test for the
 * two M12 geometric effects (warp, displacement).
 *
 * See examples/17_m12_color_quartet/main.c for shape rationale.
 *
 * Note on `displacement.texture_uri`: the JSON references
 * `file:///tmp/displacement.png` which is a placeholder URI.
 * The loader accepts any string for the URI; the texture is
 * only decoded at render time (which this example does not
 * exercise). Per-kernel pixel correctness lives in
 * `tests/test_displacement_kernel_pixel.cpp`.
 *
 * Usage:
 *   20_m12_geometric_pair [timeline.json]
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
    fprintf(stderr, "20_m12_geometric_pair — loading %s\n", path);

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
            "  loaded — warp + displacement all parsed successfully\n");

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return 0;
}
