/*
 * Typed codec selection + per-codec options for `me_output_spec_t`.
 *
 * Background. Until cycle 47, `me_output_spec_t.video_codec` /
 * `audio_codec` were `const char*` strings ("h264", "aac",
 * "hevc-sw" etc.) and the orchestrator dispatched via strcmp /
 * is_xxx_spec helpers. Each new codec needed a new helper + a
 * new strcmp branch, and per-codec parameters (bitrate especially)
 * shared a single `video_bitrate_bps` field across all video
 * codecs without partition. `docs/PAIN_POINTS.md` 2026-04-22
 * recorded the threshold; backlog
 * `me-output-spec-typed-codec-enum` calls for the typed
 * replacement.
 *
 * This header is the typed-side public ABI: enum-based codec
 * identification + per-codec opt structs + a tagged-pointer
 * aggregator (`me_codec_options_t`). Hosts opt in by attaching a
 * non-NULL `me_output_spec_t.codec_options` pointer; when present
 * it takes precedence over the string fields. NULL = legacy
 * string-based path, fully supported (no behavior change). Hosts
 * built against the pre-cycle-47 `me_output_spec_t` keep working
 * without changes provided they zero-init the struct (which is
 * the documented pattern in `examples/01_passthrough` and the
 * test suites).
 *
 * Design choices.
 *
 *   - Enum, not string, for codec identity. Eliminates the
 *     strcmp ladder in every sink + makes "unknown codec"
 *     impossible to construct (compile-time exhaustive).
 *   - Per-codec opts as separate structs (h264 / hevc /
 *     hevc_sw / aac) rather than a single tagged union. C
 *     structs without sum-type discrimination keep the API
 *     navigable to plain-C hosts; the union-shape lives one
 *     level up in `me_codec_options_t` via the typed pointer
 *     fields. Each opts struct is a POD with `bitrate_bps`
 *     and per-codec knobs; `0` means "engine default" the same
 *     way the legacy `video_bitrate_bps` does.
 *   - No `extensions_size` / version field on me_codec_options_t.
 *     C struct evolution rules in §3a.10: append-only at end of
 *     struct + add new helpers for breaking changes. When the
 *     next codec arrives the new struct lands at the end of
 *     this aggregator; older hosts that don't initialize it
 *     leave the field NULL.
 *   - Pointer-typed opts (e.g. `const me_h264_opts_t* h264`),
 *     not embedded structs. Lets hosts pass NULL for codecs
 *     they're not using + lets the engine cleanly distinguish
 *     "host wants codec X with default options" (h264 = pointer
 *     to default-zero struct) from "host doesn't use codec X"
 *     (h264 = NULL). When `video_codec` is e.g. `H264`, the
 *     engine reads `h264->bitrate_bps`; if `h264` is NULL,
 *     defaults apply.
 *
 * Migration plan. This cycle ships the public types only; sinks
 * keep the string-dispatch path. A follow-up debt bullet
 * (`debt-typed-codec-options-internal-migration`) replaces the
 * is_xxx_spec helpers with an enum-based resolver that prefers
 * `codec_options` when present and parses the strings only as
 * fallback. The two paths must produce the same outputs for the
 * same inputs.
 */
#ifndef MEDIA_ENGINE_CODEC_OPTIONS_H
#define MEDIA_ENGINE_CODEC_OPTIONS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Video codec identity.
 *
 * Order is stable: ABI-pinned per §3a.10, never re-numbered. New
 * codecs append at the end. `ME_VIDEO_CODEC_PASSTHROUGH` means
 * stream-copy when inputs are compatible; `ME_VIDEO_CODEC_NONE`
 * is "no video output" (audio-only renders).
 */
typedef enum me_video_codec {
    ME_VIDEO_CODEC_NONE        = 0,
    ME_VIDEO_CODEC_PASSTHROUGH = 1,
    ME_VIDEO_CODEC_H264        = 2,
    ME_VIDEO_CODEC_HEVC        = 3,  /* hardware (VideoToolbox on macOS, ...) */
    ME_VIDEO_CODEC_HEVC_SW     = 4   /* software fallback (Kvazaar BSD-3) */
} me_video_codec_t;

/* Audio codec identity. Same evolution rules as `me_video_codec_t`. */
typedef enum me_audio_codec {
    ME_AUDIO_CODEC_NONE        = 0,
    ME_AUDIO_CODEC_PASSTHROUGH = 1,
    ME_AUDIO_CODEC_AAC         = 2
} me_audio_codec_t;

/* Per-codec options. Each struct is POD; zero-init = "engine
 * defaults for this codec". `bitrate_bps == 0` keeps the legacy
 * "0 = codec default" semantic from `video_bitrate_bps`. */

typedef struct me_h264_opts {
    int64_t bitrate_bps;     /* 0 = codec default. */
    /* Append-only: future fields go here (profile, gop, etc.). */
} me_h264_opts_t;

typedef struct me_hevc_opts {
    int64_t bitrate_bps;
} me_hevc_opts_t;

typedef struct me_hevc_sw_opts {
    int64_t bitrate_bps;
} me_hevc_sw_opts_t;

typedef struct me_aac_opts {
    int64_t bitrate_bps;
} me_aac_opts_t;

/* Tagged-pointer aggregator. Carries the codec identity enums +
 * pointers to the per-codec opt structs.
 *
 * Selection precedence per the migration plan:
 *
 *   - `video_codec == ME_VIDEO_CODEC_NONE` (the zero-init default)
 *     means "no typed video selection — use the string path on
 *     `me_output_spec_t.video_codec`."
 *   - Any other enum value takes precedence over the string
 *     field. The matching opts pointer (e.g. `h264` when
 *     `video_codec == ME_VIDEO_CODEC_H264`) supplies the
 *     per-codec params; when NULL, codec defaults apply.
 *
 * Same precedence applies to `audio_codec` / `aac`.
 *
 * Lifetime: the host owns the aggregator + every opts struct it
 * points at. Both must outlive the `me_render_start` call —
 * they're consulted synchronously at sink-factory time, but
 * NEVER held by the engine after the sink is constructed. Stack
 * allocation in the host is safe.
 */
typedef struct me_codec_options {
    me_video_codec_t          video_codec;
    me_audio_codec_t          audio_codec;

    const me_h264_opts_t*     h264;       /* nullable; consulted iff video_codec==H264 */
    const me_hevc_opts_t*     hevc;       /* nullable; consulted iff video_codec==HEVC */
    const me_hevc_sw_opts_t*  hevc_sw;    /* nullable; consulted iff video_codec==HEVC_SW */
    const me_aac_opts_t*      aac;        /* nullable; consulted iff audio_codec==AAC */
} me_codec_options_t;

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_CODEC_OPTIONS_H */
