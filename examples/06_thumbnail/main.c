/*
 * 06_thumbnail — decode a single frame at a given time and write PNG.
 *
 * Usage:
 *   06_thumbnail <input-uri> <time-seconds> <max-w> <max-h> <output.png>
 *
 *   time-seconds is parsed as "num/den" (e.g. "15/10" = 1.5s) so the C
 *   example stays float-free on the command line, matching the engine's
 *   rational-time invariant.
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_rational(const char* s, me_rational_t* out) {
    const char* slash = strchr(s, '/');
    if (!slash) {
        out->num = atoll(s);
        out->den = 1;
        return 0;
    }
    char head[64];
    size_t n = (size_t)(slash - s);
    if (n >= sizeof(head)) return -1;
    memcpy(head, s, n);
    head[n] = 0;
    out->num = atoll(head);
    out->den = atoll(slash + 1);
    if (out->den <= 0) return -1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <uri> <time-num/den> <max-w> <max-h> <out.png>\n", argv[0]);
        return 2;
    }
    me_rational_t t = {0, 1};
    if (parse_rational(argv[2], &t) != 0) {
        fprintf(stderr, "bad time: %s\n", argv[2]);
        return 2;
    }
    int max_w = atoi(argv[3]);
    int max_h = atoi(argv[4]);

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(NULL, &eng);
    if (s != ME_OK) { fprintf(stderr, "engine_create: %s\n", me_status_str(s)); return 1; }

    uint8_t* png = NULL;
    size_t   sz  = 0;
    s = me_thumbnail_png(eng, argv[1], t, max_w, max_h, &png, &sz);
    if (s != ME_OK) {
        fprintf(stderr, "thumbnail: %s (%s)\n", me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    FILE* f = fopen(argv[5], "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s for writing\n", argv[5]);
        me_buffer_free(png);
        me_engine_destroy(eng);
        return 1;
    }
    size_t wrote = fwrite(png, 1, sz, f);
    fclose(f);
    me_buffer_free(png);
    me_engine_destroy(eng);

    if (wrote != sz) {
        fprintf(stderr, "short write: %zu of %zu\n", wrote, sz);
        return 1;
    }
    fprintf(stderr, "wrote %zu bytes → %s\n", sz, argv[5]);
    return 0;
}
