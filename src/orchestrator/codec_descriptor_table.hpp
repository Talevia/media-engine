/* codec_descriptor_table — single source of truth mapping
 * `me_video_codec_t` enum values to (legacy-string, avcodec
 * encoder name, AVPixelFormat, default bitrate).
 *
 * Before this header existed, the same mapping was split across
 * two files:
 *   - `codec_resolver.cpp`'s `parse_video_codec_string` (string
 *     → enum)
 *   - `reencode_video.cpp`'s switch on `video_codec_enum` (enum
 *     → encoder name + pix_fmt + default bitrate)
 *
 * Adding a new codec meant editing both. With the unified table
 * here a new entry covers both directions; the resolver and
 * the reencoder consult the same array. M7-debt cross-milestone
 * cleanup (`debt-codec-dispatch-table-unification`).
 *
 * The table is a constexpr array indexed via linear search by
 * enum or string; the lookups are O(N) on N=5 entries which
 * fits in a cache line — no win from hashing.
 *
 * `avcodec_encoder_name` is `nullptr` for sentinel enum values
 * that don't have a real encoder (NONE, PASSTHROUGH). Callers
 * should treat null as "this enum is not an encoder" — i.e.
 * route to a different sink path.
 *
 * `pix_fmt` is `AV_PIX_FMT_NONE` for the same sentinel cases.
 * `default_bitrate_bps` is 0 for sentinels (and means "engine
 * default" generally for real encoders too — see CodecSelection
 * precedence). */
#pragma once

#include "media_engine/codec_options.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <span>

namespace me::orchestrator {

struct VideoCodecDescriptor {
    me_video_codec_t enum_value;
    /* Legacy string accepted by `me_output_spec_t::video_codec`.
     * Always non-null; "" maps to NONE elsewhere (in the
     * resolver) before hitting the lookup. */
    const char*      legacy_string;
    /* libavcodec encoder name passed to avcodec_find_encoder_by_name.
     * `nullptr` = this enum is not a real encoder (NONE / PASSTHROUGH);
     * the reencoder rejects with ME_E_UNSUPPORTED. */
    const char*      avcodec_encoder_name;
    /* libavcodec input pixel format expected by the encoder.
     * AV_PIX_FMT_NONE for sentinel enums. */
    AVPixelFormat    pix_fmt;
    /* Engine-default bitrate when the caller passes 0 (the
     * documented "engine default" sentinel). 0 here for sentinel
     * enums. */
    std::int64_t     default_bitrate_bps;
};

/* Returns a span over the static descriptor table. The order is
 * stable: index = enum value (verified by a static_assert in the
 * .cpp). */
std::span<const VideoCodecDescriptor> video_codec_descriptors();

/* Linear lookup: parse a string into the matching enum value,
 * returning ME_VIDEO_CODEC_NONE for null / empty / "none" / any
 * unrecognized name. Same semantics as the previous
 * `parse_video_codec_string` in codec_resolver.cpp. */
me_video_codec_t lookup_video_codec_by_string(const char* s) noexcept;

/* Returns the descriptor for `enum_value`, or nullptr if the
 * enum is out of the table's range. The current table covers
 * NONE / PASSTHROUGH / H264 / HEVC / HEVC_SW; future codec
 * additions append to the table and bump the enum tail. */
const VideoCodecDescriptor* lookup_video_codec_by_enum(
    me_video_codec_t enum_value) noexcept;

}  // namespace me::orchestrator
