/*
 * Re-encode pipeline — Exporter's non-passthrough specialization.
 *
 * Consumes N opened io::DemuxContexts (same sources as passthrough_mux)
 * and runs each segment's packets through decoder → (optional pixfmt /
 * samplefmt convert) → encoder → output mux. Scope for M1: video_codec =
 * "h264" on macOS (h264_videotoolbox) + audio_codec = "aac" (libavcodec
 * built-in AAC, LGPL-clean). Stream selection mirrors passthrough_mux
 * (best video + best audio; other tracks dropped).
 *
 * Multi-segment: one shared output encoder runs across all segments; each
 * segment opens its own per-source decoder, scales / resamples into the
 * encoder's native format, and its frames are stamped with a continuous
 * output-timeline PTS (video via running counter + fixed CFR delta, audio
 * via the cross-segment AAC-sized FIFO drain). Segment boundaries flush
 * the per-segment decoder into the shared encoder; the encoder itself is
 * only flushed at the end of the last segment. Subsequent segments must
 * present codec params compatible with the first segment — same rationale
 * as passthrough_mux's codecpar_compatible check.
 *
 * Not a graph Node: encoder state lives across many frames (and now many
 * segments), which violates the per-frame-pure Node contract
 * (ARCHITECTURE_GRAPH.md §批编码). Sits in orchestrator alongside
 * muxer_state for the same reason passthrough_mux does.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"   /* me::ColorSpace */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace me::io       { class DemuxContext; }
namespace me::resource { class CodecPool; }

namespace me::orchestrator {

/* One input segment to re-encode. Parallel to muxer_state.hpp's
 * PassthroughSegment — the two flows share shape so future orchestrator
 * layers (composition, multi-track) can build them identically. */
struct ReencodeSegment {
    std::shared_ptr<me::io::DemuxContext> demux;   /* opened + outlives reencode_mux */
    me_rational_t                         source_start    { 0, 1 };
    me_rational_t                         source_duration { 0, 1 };  /* 0 = to EOF */
    /* Per-clip source color space, from the Asset the clip references
     * (see `me::Asset::color_space` populated by the timeline loader's
     * `asset-colorspace-field` path). Default-constructed ColorSpace
     * means UNSPECIFIED — when `IdentityPipeline` is the active
     * pipeline this is a no-op; when OCIO is active the pipeline
     * treats UNSPECIFIED as "assume timeline's target color space",
     * i.e. skip conversion. */
    me::ColorSpace                        source_color_space {};
};

struct ReencodeOptions {
    std::string out_path;
    std::string container;          /* empty = infer from extension */

    /* Video: currently only "h264" is accepted (mapped to h264_videotoolbox). */
    std::string video_codec;
    int64_t     video_bitrate_bps = 0;    /* 0 = encoder default */

    /* Audio: currently only "aac" is accepted (libavcodec built-in). */
    std::string audio_codec;
    int64_t     audio_bitrate_bps = 0;    /* 0 = encoder default */

    std::function<void(float)> on_ratio;
    const std::atomic<bool>*   cancel = nullptr;

    /* Required: engine's codec pool. All AVCodecContext allocations
     * (decoders + encoders) run through this so `me_cache_stats.codec_ctx_count`
     * reports live reality. */
    me::resource::CodecPool*   pool   = nullptr;

    /* >= 1 entry; segment 0 determines the output encoder's width /
     * height / pix_fmt / sample_rate / ch_layout. Subsequent segments
     * with incompatible codec params are rejected. */
    std::vector<ReencodeSegment> segments;

    /* Timeline-level working / output color space (from
     * `me::Timeline::color_space`). Threaded into `SharedEncState` so
     * `process_segment` can hand the real `(src, dst)` pair to
     * `me::color::Pipeline::apply()`. Default ColorSpace means
     * UNSPECIFIED (identity / no-op on the IdentityPipeline path). */
    me::ColorSpace target_color_space {};
};

/* Process all segments in order into a single output container. Returns
 * terminal status; human-readable diagnostic written to *err on failure. */
me_status_t reencode_mux(const ReencodeOptions&   opts,
                         std::string*             err);

}  // namespace me::orchestrator
