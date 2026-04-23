/*
 * MuxerState — Exporter-owned streaming output state.
 *
 * Writes one or more DemuxContext inputs through to a single output
 * container via stream-copy (passthrough). Multi-segment concat is handled
 * here rather than at the graph layer because muxing is inherently
 * stateful across many frames (ARCHITECTURE_GRAPH.md §批编码).
 *
 * All segments must share a compatible codec parameter set (codec_id,
 * profile, width/height, extradata). Otherwise stream-copy would emit a
 * header that disagrees with later bitstream — passthrough rejects the
 * mismatch at mux-open time.
 */
#pragma once

#include "media_engine/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace me::io { class DemuxContext; }

namespace me::orchestrator {

struct PassthroughSegment {
    /* Opened demuxer — owner is the caller; must outlive passthrough_mux. */
    std::shared_ptr<me::io::DemuxContext> demux;

    /* Offset into the source asset where this segment's content begins.
     * With stream-copy, the output will start at the nearest prior
     * keyframe (GOP-rounded); users who need sample-accurate trimming
     * must use the re-encode path. {0, 1} = from start. */
    me_rational_t source_start{0, 1};

    /* Duration taken from the source; {0, 1} = until EOF. */
    me_rational_t source_duration{0, 1};

    /* Advisory — the output is DTS-continuity-enforced rather than
     * offset-driven. Kept on the struct so callers (Exporter) can carry
     * the timeline's timeRange.start through, and for future schemas
     * that allow gaps. Phase-1 ignores this field. */
    me_rational_t time_offset{0, 1};
};

struct PassthroughMuxOptions {
    std::string out_path;
    std::string container;                         /* empty = infer from extension */
    std::vector<PassthroughSegment> segments;      /* ≥1, in output order */
    std::function<void(float)> on_ratio;           /* optional per-packet progress */
    const std::atomic<bool>*    cancel = nullptr;
};

/* Open an output file, copy every segment's packets in order (rebasing PTS
 * so the output timeline is contiguous), write the trailer, return the
 * terminal status. On failure, writes a human-readable diagnostic to *err. */
me_status_t passthrough_mux(const PassthroughMuxOptions&  opts,
                            std::string*                  err);

}  // namespace me::orchestrator
