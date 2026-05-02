/*
 * `me::orchestrator::resolve_codec_selection` — internal classifier
 * that turns the dual-shape `me_output_spec_t` (legacy strings +
 * cycle-47 typed `codec_options` extension) into a single typed
 * `CodecSelection` for the sink-factory + sink-internal dispatch.
 *
 * M7-debt cross-milestone follow-up to cycle 0d53971's typed-codec
 * ABI landing (include/media_engine/codec_options.h). The bullet
 * `debt-typed-codec-options-internal-migration` calls for replacing
 * the per-sink `is_xxx_spec` helpers + `streq("h264")` ladders
 * scattered across `output_sink.cpp` / `compose_sink.cpp` /
 * `audio_only_sink.cpp` / `hevc_sw_sink.cpp` with a single typed
 * resolver that the sinks consult.
 *
 * Precedence rules (matching the public `me_codec_options_t`
 * contract documented in `include/media_engine/codec_options.h`):
 *
 *   - When `spec.codec_options == nullptr` OR
 *     `spec.codec_options->video_codec == ME_VIDEO_CODEC_NONE`,
 *     parse the legacy `spec.video_codec` string into the enum.
 *     Same precedence for audio.
 *   - Otherwise the typed enum wins; the matching per-codec opts
 *     pointer (e.g. `codec_options->h264` when `video_codec ==
 *     ME_VIDEO_CODEC_H264`) supplies the bitrate. NULL opts ptr
 *     falls back to `spec.video_bitrate_bps` (which itself
 *     defaults to 0 = "engine default").
 *
 * String-to-enum mapping (legacy path):
 *
 *   "passthrough"  → ME_VIDEO_CODEC_PASSTHROUGH / ME_AUDIO_CODEC_PASSTHROUGH
 *   "h264"         → ME_VIDEO_CODEC_H264
 *   "hevc"         → ME_VIDEO_CODEC_HEVC
 *   "hevc-sw"      → ME_VIDEO_CODEC_HEVC_SW
 *   "aac"          → ME_AUDIO_CODEC_AAC
 *   NULL / "" / "none" → ME_VIDEO_CODEC_NONE / ME_AUDIO_CODEC_NONE
 *   any other      → ME_VIDEO_CODEC_NONE / ME_AUDIO_CODEC_NONE
 *                     (sinks reject unknown codec names with their
 *                      existing diagnostic messages; the resolver
 *                      itself doesn't fail on unknown strings —
 *                      that's the dispatch layer's job).
 *
 * Determinism: pure-function classifier, no I/O, no allocation.
 * Same input → same output.
 */
#pragma once

#include "media_engine/codec_options.h"
#include "media_engine/render.h"

#include <cstdint>

namespace me::orchestrator {

struct CodecSelection {
    me_video_codec_t video_codec      = ME_VIDEO_CODEC_NONE;
    me_audio_codec_t audio_codec      = ME_AUDIO_CODEC_NONE;
    /* Resolved bitrate (per-codec opts.bitrate_bps when present and
     * non-zero, else falls back to spec.video_bitrate_bps /
     * spec.audio_bitrate_bps). 0 = "engine default for this
     * codec" — the same sentinel the legacy path uses. */
    std::int64_t     video_bitrate_bps = 0;
    std::int64_t     audio_bitrate_bps = 0;
};

CodecSelection resolve_codec_selection(const me_output_spec_t& spec);

}  // namespace me::orchestrator
