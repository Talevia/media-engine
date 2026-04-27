/*
 * Media probe — inspect a single media file without decoding its content.
 *
 * probe is cheap and side-effect free. It may read file headers over the
 * network when the URI is http(s); results are not cached by default.
 */
#ifndef MEDIA_ENGINE_PROBE_H
#define MEDIA_ENGINE_PROBE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

ME_API me_status_t me_probe(me_engine_t* engine, const char* uri, me_media_info_t** out);
ME_API void        me_media_info_destroy(me_media_info_t* info);

/* Strings returned below live as long as `info`. */
ME_API const char*   me_media_info_container(const me_media_info_t* info);
ME_API me_rational_t me_media_info_duration(const me_media_info_t* info);

/* Video stream (0 if none). */
ME_API int           me_media_info_has_video(const me_media_info_t* info);
ME_API int           me_media_info_video_width(const me_media_info_t* info);
ME_API int           me_media_info_video_height(const me_media_info_t* info);
ME_API me_rational_t me_media_info_video_frame_rate(const me_media_info_t* info);
ME_API const char*   me_media_info_video_codec(const me_media_info_t* info);

/* Audio stream (0 if none). */
ME_API int           me_media_info_has_audio(const me_media_info_t* info);
ME_API int           me_media_info_audio_sample_rate(const me_media_info_t* info);
ME_API int           me_media_info_audio_channels(const me_media_info_t* info);
ME_API const char*   me_media_info_audio_codec(const me_media_info_t* info);

/* --- Extended video metadata (append-only since 0.0.2) --------------------
 *
 * These accessors round out the minimum M1 probe surface (container / codec
 * / dimensions / frame rate / sample rate / channels) with the fields M2's
 * compose + color-management paths need to decide between identity copy and
 * real pixel conversion:
 *
 *   - video_rotation: integer degrees derived from the container-level
 *     display matrix (AV_PKT_DATA_DISPLAYMATRIX), normalised to one of
 *     {0, 90, 180, 270}. iOS portrait captures routinely ship 90° rotation
 *     metadata against a landscape pixel buffer. 0 when absent or malformed.
 *   - video_color_range: "tv" (limited / MPEG range, 16-235) or "pc"
 *     (full / JPEG range, 0-255); "unspecified" when the container is silent.
 *     BT.709 wins over BT.2020 only when you apply the right range offset —
 *     wrong range = consistent dark or washed output.
 *   - video_color_primaries / transfer / space: container-declared chromaticity
 *     primaries, transfer function, and YCbCr→RGB matrix (e.g. "bt709",
 *     "bt2020", "smpte2084", "arib-std-b67"). Empty string when absent.
 *     OCIO needs all three to produce the correct working-space conversion.
 *   - video_bit_depth: bits per luminance channel (8 / 10 / 12). Derived
 *     from the pixel format descriptor, not the codec parameters, so HDR10
 *     (10-bit Main10) and SDR 8-bit paths can diverge early in the pipeline.
 *     0 when the pixel format is unknown.
 *
 * These are append-only — existing accessors keep their signatures and
 * semantics. Callers compiled against 0.0.1 headers link-check and run
 * unchanged. */
ME_API int           me_media_info_video_rotation(const me_media_info_t* info);
ME_API const char*   me_media_info_video_color_range(const me_media_info_t* info);
ME_API const char*   me_media_info_video_color_primaries(const me_media_info_t* info);
ME_API const char*   me_media_info_video_color_transfer(const me_media_info_t* info);
ME_API const char*   me_media_info_video_color_space(const me_media_info_t* info);
ME_API int           me_media_info_video_bit_depth(const me_media_info_t* info);

/* --- HDR static metadata (append-only since 0.0.3) ------------------------
 *
 * HDR10 / SMPTE ST 2086 mastering display metadata + CTA-861.3 content
 * light level info, when the container declares them on the video
 * stream. Independently optional — a stream may carry one, both, or
 * neither.
 *
 * Fields below are valid only when the corresponding `has_*` flag is
 * non-zero. All-zero (default-initialised) means the container did
 * not advertise HDR static metadata.
 *
 *   has_mastering_display:
 *     The `mdcv_*` fields carry SMPTE ST 2086 primaries + luminance
 *     bounds, read from `AV_PKT_DATA_MASTERING_DISPLAY_METADATA` on
 *     the video stream's coded side data. Set only when libavutil's
 *     `AVMasteringDisplayMetadata` reports BOTH `has_primaries` and
 *     `has_luminance` — a half-populated struct is treated as
 *     unreliable (downstream HDR pipelines need the full set to
 *     drive a tonemap or pass-through correctly).
 *   mdcv_red_x..mdcv_white_y:
 *     CIE 1931 chromaticities for the mastering display's R / G / B
 *     primaries and white point, normalised to [0, 1]. Stored as
 *     rationals (libavutil's exact `AVRational` values; no float
 *     rounding).
 *   mdcv_min_luminance / mdcv_max_luminance:
 *     Mastering display luminance bounds in cd/m² (= nits).
 *     Rational-typed for the same reason.
 *
 *   has_content_light:
 *     `max_cll` and `max_fall` carry CTA-861.3 max content light
 *     level (peak per-pixel) and max frame-average light level, both
 *     in nits. Read from `AV_PKT_DATA_CONTENT_LIGHT_LEVEL`. A zero
 *     value within an enabled struct means the container declared
 *     the field as zero (rare but legal).
 *
 * Returned by-value from `me_media_info_video_hdr_metadata`. The
 * accessor is safe to call when `info` is NULL or the stream has no
 * video — it returns an all-zero struct in those cases. */
typedef struct me_hdr_static_metadata {
    int           has_mastering_display;
    me_rational_t mdcv_red_x;
    me_rational_t mdcv_red_y;
    me_rational_t mdcv_green_x;
    me_rational_t mdcv_green_y;
    me_rational_t mdcv_blue_x;
    me_rational_t mdcv_blue_y;
    me_rational_t mdcv_white_x;
    me_rational_t mdcv_white_y;
    me_rational_t mdcv_min_luminance;
    me_rational_t mdcv_max_luminance;

    int           has_content_light;
    int           max_cll;
    int           max_fall;
} me_hdr_static_metadata_t;

ME_API me_hdr_static_metadata_t me_media_info_video_hdr_metadata(const me_media_info_t* info);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_PROBE_H */
