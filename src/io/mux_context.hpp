/*
 * MuxContext — output-side AVFormatContext wrapped in RAII.
 *
 * Output-side symmetric counterpart to `io::DemuxContext`. Owns the
 * `avformat_alloc_output_context2 → avio_open → write_header → write_trailer
 * → avio_closep → free_context` chain. The destructor always runs the
 * teardown half of the chain regardless of which phase(s) the caller
 * reached, so error paths in the muxer pipelines don't need to copy-paste
 * a manual cleanup lambda into every branch.
 *
 * Typical usage:
 *   auto mux = me::io::MuxContext::open(path, container, &err);
 *   if (!mux) return status_from(err);
 *   // configure streams through mux->fmt():
 *   //   avformat_new_stream, avcodec_parameters_copy, ...
 *   if (auto s = mux->open_avio(&err);    s != ME_OK) return s;
 *   if (auto s = mux->write_header(&err); s != ME_OK) return s;
 *   // loop: mux->write_and_track(pkt, last_end_per_stream, &err);
 *   if (auto s = mux->write_trailer(&err); s != ME_OK) return s;
 *   // destructor: if write_trailer wasn't reached, still closes avio +
 *   // frees the AVFormatContext.
 *
 * Not thread-safe — each MuxContext is owned by exactly one muxing pipeline
 * at a time.
 */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct AVFormatContext;
struct AVPacket;

namespace me::io {

class MuxContext {
public:
    /* Allocate + initialize an output AVFormatContext. container may be
     * empty (inferred from out_path extension). Returns nullptr on failure
     * with a diagnostic written to *err. */
    static std::unique_ptr<MuxContext> open(std::string_view out_path,
                                             std::string_view container,
                                             std::string*     err);

    ~MuxContext();

    MuxContext(const MuxContext&) = delete;
    MuxContext& operator=(const MuxContext&) = delete;

    AVFormatContext*       fmt()       noexcept { return fmt_; }
    const AVFormatContext* fmt() const noexcept { return fmt_; }

    const std::string& out_path() const noexcept { return out_path_; }

    /* Open avio for writing (if the container needs a file — AVFMT_NOFILE
     * formats skip this). Idempotent on success: a second call is a no-op.
     * Returns ME_E_IO on failure. */
    me_status_t open_avio(std::string* err);

    /* Write the container header. Must be called once, after all streams
     * are configured and open_avio() has succeeded (if applicable). */
    me_status_t write_header(std::string* err);

    /* Write the trailer. Must be called once, after all packets have been
     * written. If the caller bails on an error and never reaches this,
     * the destructor still frees the context cleanly — it just doesn't
     * emit a trailer, which is correct (the file is abandoned). */
    me_status_t write_trailer(std::string* err);

    /* Snapshot-before-write helper that captures the common concat
     * bookkeeping pattern from PAIN_POINTS:
     *   - Computes `pkt->pts + pkt->duration` BEFORE the write call,
     *     because av_interleaved_write_frame takes ownership of pkt and
     *     zeroes its fields.
     *   - On success, advances `last_end_per_stream[pkt->stream_index]`
     *     monotonically with that snapshot.
     *   - Always unrefs pkt regardless of outcome.
     * If last_end_per_stream is smaller than nb_streams, it is resized.
     * Returns ME_E_ENCODE on write failure with diagnostic in *err. */
    me_status_t write_and_track(AVPacket*              pkt,
                                std::vector<int64_t>&  last_end_per_stream,
                                std::string*           err);

private:
    MuxContext() = default;

    AVFormatContext* fmt_             = nullptr;
    bool             avio_opened_     = false;
    bool             header_written_  = false;
    bool             trailer_written_ = false;
    std::string      out_path_;
};

}  // namespace me::io
