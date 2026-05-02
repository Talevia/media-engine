/*
 * HevcSwSink — single-segment SW HEVC encode using Kvazaar
 * (BSD-3, LGPL-clean per VISION §3.4) with raw Annex-B output via
 * libavformat's "hevc" muxer.
 *
 * Completes the dispatch wired by
 * `encode-hevc-output-sink-runtime-sw-dispatch` (prior cycle): the
 * dispatch piece in `open_video_encoder` performs preflight + emits
 * a "encode-loop wiring pending" diagnostic; this sink is the encode
 * loop. Spec match: `video_codec == "hevc-sw"` paired with
 * `audio_codec == "none"` (or NULL/empty). MP4 + AAC integration
 * (hvcC extradata extraction, SharedEncState refactor for
 * polymorphic encoder, multi-segment) is the
 * `debt-hevc-sw-mp4-mux-with-aac` follow-up.
 *
 * Constraints (M10 §3 SW path is "limited output"):
 *   - Single segment. Multi-segment SW HEVC concat would need either
 *     re-init of Kvazaar per segment (loses GOP continuity) or
 *     bitstream concat with cross-segment reordering which Kvazaar
 *     doesn't expose.
 *   - 1920x1080 ceiling, multiple-of-8 alignment (enforced by
 *     `KvazaarHevcEncoder::create`).
 *   - Video-only. Audio is rejected at spec match by
 *     `is_hevc_sw_video_only_spec`.
 *   - Container: raw Annex-B HEVC ("hevc"). The output file extension
 *     is the caller's responsibility — `.hevc` / `.h265` are
 *     conventional.
 *   - Determinism: per VISION §3.4 + KvazaarHevcEncoder header,
 *     deterministic within a single Kvazaar build + host arch +
 *     encoding parameters; not byte-identical across libraries.
 */
#pragma once

#include "orchestrator/output_sink.hpp"

namespace me::orchestrator {

/* True iff `spec` matches the (hevc-sw, no-audio) shape this sink
 * accepts: `video_codec == "hevc-sw"` and `audio_codec` is either
 * NULL, empty string, or `"none"`. The output_sink factory consults
 * this before falling through to `is_video_aac_spec` (which would
 * route hevc-sw + aac to the VideoAacSink that returns
 * ME_E_UNSUPPORTED via open_video_encoder's preflight). */
bool is_hevc_sw_video_only_spec(const me_output_spec_t& spec);

/* Build the sink. Caller owns the returned pointer; lifetime is
 * tied to the Exporter's worker thread. `clip_ranges` must contain
 * exactly one entry (the single segment). */
std::unique_ptr<OutputSink> make_hevc_sw_sink(
    SinkCommon                 common,
    std::vector<ClipTimeRange> clip_ranges);

}  // namespace me::orchestrator
