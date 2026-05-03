/* codec_resolver impl. See header for the contract.
 *
 * Implementation note: every codec-name string compare here MUST
 * match the strings the legacy path accepted, or migrating an
 * existing sink to the resolver would silently change behavior.
 * The reference list is `src/orchestrator/output_sink.cpp`'s
 * `is_passthrough_spec` / `is_video_aac_spec` plus the inlined
 * `(HEVC_SW, NONE)` shape match in `make_output_sink`. Any
 * string additions there require matching additions here.
 */
#include "orchestrator/codec_resolver.hpp"

#include "orchestrator/codec_descriptor_table.hpp"

#include <string_view>

namespace me::orchestrator {

namespace {

me_audio_codec_t parse_audio_codec_string(const char* s) noexcept {
    if (!s || s[0] == '\0') return ME_AUDIO_CODEC_NONE;
    const std::string_view sv{s};
    if (sv == "none")        return ME_AUDIO_CODEC_NONE;
    if (sv == "passthrough") return ME_AUDIO_CODEC_PASSTHROUGH;
    if (sv == "aac")         return ME_AUDIO_CODEC_AAC;
    return ME_AUDIO_CODEC_NONE;
}

}  // namespace

CodecSelection resolve_codec_selection(const me_output_spec_t& spec) {
    CodecSelection out{};

    /* Phase 1: pick the typed video codec, preferring codec_options
     * when its enum is non-NONE per the public precedence contract. */
    if (spec.codec_options &&
        spec.codec_options->video_codec != ME_VIDEO_CODEC_NONE) {
        out.video_codec = spec.codec_options->video_codec;
    } else {
        out.video_codec = lookup_video_codec_by_string(spec.video_codec);
    }

    /* Phase 2: same for audio. */
    if (spec.codec_options &&
        spec.codec_options->audio_codec != ME_AUDIO_CODEC_NONE) {
        out.audio_codec = spec.codec_options->audio_codec;
    } else {
        out.audio_codec = parse_audio_codec_string(spec.audio_codec);
    }

    /* Phase 3: resolve per-codec bitrates. The per-codec opts
     * pointer takes precedence ONLY when it's attached AND its
     * bitrate_bps is non-zero (the documented "0 = engine default"
     * sentinel from codec_options.h). When it's attached with
     * bitrate_bps == 0, OR when the pointer is NULL, fall back to
     * `spec.video_bitrate_bps` (which itself defaults to 0). */
    out.video_bitrate_bps = spec.video_bitrate_bps;
    if (spec.codec_options) {
        switch (out.video_codec) {
        case ME_VIDEO_CODEC_H264:
            if (spec.codec_options->h264 &&
                spec.codec_options->h264->bitrate_bps > 0) {
                out.video_bitrate_bps = spec.codec_options->h264->bitrate_bps;
            }
            break;
        case ME_VIDEO_CODEC_HEVC:
            if (spec.codec_options->hevc &&
                spec.codec_options->hevc->bitrate_bps > 0) {
                out.video_bitrate_bps = spec.codec_options->hevc->bitrate_bps;
            }
            break;
        case ME_VIDEO_CODEC_HEVC_SW:
            if (spec.codec_options->hevc_sw &&
                spec.codec_options->hevc_sw->bitrate_bps > 0) {
                out.video_bitrate_bps = spec.codec_options->hevc_sw->bitrate_bps;
            }
            break;
        case ME_VIDEO_CODEC_NONE:
        case ME_VIDEO_CODEC_PASSTHROUGH:
            /* No per-codec opts apply (passthrough has no encoder
             * to configure; NONE has no codec at all). */
            break;
        }
    }

    out.audio_bitrate_bps = spec.audio_bitrate_bps;
    if (spec.codec_options &&
        out.audio_codec == ME_AUDIO_CODEC_AAC &&
        spec.codec_options->aac &&
        spec.codec_options->aac->bitrate_bps > 0) {
        out.audio_bitrate_bps = spec.codec_options->aac->bitrate_bps;
    }

    return out;
}

}  // namespace me::orchestrator
