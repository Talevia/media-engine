/*
 * Re-encode pipeline — Exporter's non-passthrough specialization.
 *
 * Consumes an opened io::DemuxContext (same source as passthrough_mux)
 * but runs packets through decoder → (optional pixfmt/samplefmt convert) →
 * encoder → output mux. Scope for M1: video_codec="h264" on macOS
 * (h264_videotoolbox) + audio_codec="aac" (libavcodec built-in AAC,
 * LGPL-clean). Stream selection mirrors passthrough_mux (best video +
 * best audio; other tracks dropped).
 *
 * Not a graph Node: encoder state lives across many frames, which
 * violates the per-frame-pure Node contract (ARCHITECTURE_GRAPH.md
 * §批编码). Sits in orchestrator alongside muxer_state for the same
 * reason passthrough_mux does.
 */
#pragma once

#include "media_engine/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace me::io { class DemuxContext; }
namespace me::resource { class CodecPool; }

namespace me::orchestrator {

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
};

/* Read packets from `demux`, decode + re-encode, write to a new container.
 * Returns terminal status; human-readable diagnostic written to *err on
 * failure. */
me_status_t reencode_mux(io::DemuxContext&        demux,
                         const ReencodeOptions&   opts,
                         std::string*             err);

}  // namespace me::orchestrator
