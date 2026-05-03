/*
 * 17_m12_color_quartet — JSON loader smoke test for the four M12
 * color effects (tone_curve, hue_saturation, vignette, film_grain).
 *
 * This example does NOT render — it loads the timeline JSON via
 * the public C API, asserts the load succeeded, and exits. The
 * rendering correctness of each effect is covered by the per-
 * kernel pixel tests under tests/test_<kind>_kernel_pixel.cpp.
 *
 * Purpose: M12 §160 timeline-JSON-example coverage. This is the
 * tripwire for the loader's per-EffectKind dispatch — if a future
 * refactor breaks the JSON parser for these four kinds, this
 * example fails to configure / build / load.
 *
 * Usage:
 *   17_m12_color_quartet [timeline.json]
 *
 * If no path is given, defaults to the COPY_RESOURCE'd
 * sample.timeline.json in the same directory as the binary.
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
    fprintf(stderr, "17_m12_color_quartet — loading %s\n", path);

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
            "  loaded — tone_curve + hue_saturation + vignette + film_grain "
            "all parsed successfully\n");

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return 0;
}
