/*
 * 04_probe — dump container / stream metadata for a single media URI.
 *
 * Purpose: exercise me_probe + me_media_info_* from a plain C TU, so the
 * extern "C" boundary is compile-checked and the accessors are visible
 * end-to-end via the public header surface.
 *
 * Usage:
 *   04_probe <path-or-uri>
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>

static double rat_to_double(me_rational_t r) {
    return r.den ? (double)r.num / (double)r.den : 0.0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-or-uri>\n", argv[0]);
        return 2;
    }

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(NULL, &eng);
    if (s != ME_OK) {
        fprintf(stderr, "engine_create: %s\n", me_status_str(s));
        return 1;
    }

    me_media_info_t* info = NULL;
    s = me_probe(eng, argv[1], &info);
    if (s != ME_OK) {
        fprintf(stderr, "probe: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    me_rational_t dur = me_media_info_duration(info);
    printf("container     : %s\n", me_media_info_container(info));
    printf("duration      : %lld/%lld (~%.3fs)\n",
           (long long)dur.num, (long long)dur.den, rat_to_double(dur));

    if (me_media_info_has_video(info)) {
        me_rational_t fr = me_media_info_video_frame_rate(info);
        printf("video         : %s  %dx%d  @ %lld/%lld (~%.3f fps)\n",
               me_media_info_video_codec(info),
               me_media_info_video_width(info),
               me_media_info_video_height(info),
               (long long)fr.num, (long long)fr.den, rat_to_double(fr));
        printf("  rotation    : %d°\n", me_media_info_video_rotation(info));
        printf("  bit depth   : %d\n", me_media_info_video_bit_depth(info));
        printf("  color range : %s\n", me_media_info_video_color_range(info));
        printf("  primaries   : %s\n", me_media_info_video_color_primaries(info));
        printf("  transfer    : %s\n", me_media_info_video_color_transfer(info));
        printf("  matrix      : %s\n", me_media_info_video_color_space(info));
    } else {
        printf("video         : (none)\n");
    }

    if (me_media_info_has_audio(info)) {
        printf("audio         : %s  %d Hz  %d ch\n",
               me_media_info_audio_codec(info),
               me_media_info_audio_sample_rate(info),
               me_media_info_audio_channels(info));
    } else {
        printf("audio         : (none)\n");
    }

    me_media_info_destroy(info);
    me_engine_destroy(eng);
    return 0;
}
