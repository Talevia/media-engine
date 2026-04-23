/*
 * DemuxContext — opened AVFormatContext + stream metadata, carried as the
 * output of the io::demux kernel.
 *
 * Lives as a heap-allocated RAII wrapper that the graph's InputValue
 * variant references via shared_ptr. The orchestrator consumes it
 * downstream (currently: Exporter's passthrough mux; later: decode
 * kernels that pull packets).
 *
 * Not thread-safe — each DemuxContext is owned by exactly one reader at
 * a time. Concurrent evaluations of the same graph should each produce
 * their own DemuxContext (one per EvalInstance) until the CodecPool /
 * shared-demuxer optimization arrives.
 */
#pragma once

#include <memory>
#include <string>

struct AVFormatContext;

namespace me::io {

class DemuxContext {
public:
    DemuxContext() = default;
    ~DemuxContext();

    DemuxContext(const DemuxContext&)            = delete;
    DemuxContext& operator=(const DemuxContext&) = delete;
    DemuxContext(DemuxContext&&)                 = delete;

    AVFormatContext* fmt  = nullptr;     /* owned; closed in dtor */
    std::string      uri;                /* source URI for diagnostics */
};

}  // namespace me::io
