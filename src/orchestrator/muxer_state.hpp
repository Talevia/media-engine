/*
 * MuxerState — Exporter-owned streaming output state.
 *
 * Consumes an opened io::DemuxContext (produced by the io::demux kernel)
 * and writes its packets through to a new container, stream-copying for
 * the passthrough specialization. Separate from any Graph Node because
 * the muxing side is inherently stateful across many frames — Graph is
 * single-frame per architecture (ARCHITECTURE_GRAPH.md §批编码).
 */
#pragma once

#include "media_engine/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace me::io { class DemuxContext; }

namespace me::orchestrator {

struct PassthroughMuxOptions {
    std::string out_path;
    std::string container;       /* empty = infer from extension */
    std::function<void(float)> on_ratio;    /* optional per-packet progress */
    const std::atomic<bool>*   cancel = nullptr;
};

/* Read every packet from `demux` and write it to a new container via
 * stream copy. Returns terminal status; writes human-readable diagnostic
 * to *err on failure. */
me_status_t passthrough_mux(io::DemuxContext&             demux,
                            const PassthroughMuxOptions&  opts,
                            std::string*                  err);

}  // namespace me::orchestrator
