#include "io/mux_context.hpp"
#include "io/av_err.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <string>

namespace me::io {

std::unique_ptr<MuxContext> MuxContext::open(std::string_view out_path,
                                              std::string_view container,
                                              std::string*     err) {
    AVFormatContext* fmt = nullptr;
    const std::string out{out_path};
    const std::string ctr{container};
    const char* format_name = ctr.empty() ? nullptr : ctr.c_str();

    const int rc = avformat_alloc_output_context2(&fmt, nullptr, format_name, out.c_str());
    if (rc < 0 || !fmt) {
        if (err) *err = "alloc output: " + av_err_str(rc);
        return nullptr;
    }

    /* AVFMT_FLAG_BITEXACT zeroes mvhd/tkhd creation_time and skips
     * "encoder" / "major_brand_version" string stamping that otherwise
     * varies across libav builds. This is the precondition for byte-
     * deterministic output across software paths (see test_determinism).
     * Always-on is safe: the flag is about omitting non-essential metadata,
     * not changing stream data. */
    fmt->flags |= AVFMT_FLAG_BITEXACT;

    auto ctx      = std::unique_ptr<MuxContext>(new MuxContext());
    ctx->fmt_     = fmt;
    ctx->out_path_ = out;
    return ctx;
}

MuxContext::~MuxContext() {
    if (!fmt_) return;
    if (avio_opened_ && fmt_->pb) {
        avio_closep(&fmt_->pb);
    }
    avformat_free_context(fmt_);
    fmt_ = nullptr;
}

me_status_t MuxContext::open_avio(std::string* err) {
    if (!fmt_) {
        if (err) *err = "mux_context: no fmt";
        return ME_E_INVALID_ARG;
    }
    if (avio_opened_) return ME_OK;
    /* AVFMT_NOFILE formats (e.g. RTMP, some pipe muxers) handle their
     * own transport — the header/trailer calls just work without avio. */
    if (fmt_->oformat->flags & AVFMT_NOFILE) {
        avio_opened_ = true;   /* mark as "handled", even though no file */
        return ME_OK;
    }
    const int rc = avio_open(&fmt_->pb, out_path_.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        if (err) *err = "avio_open: " + av_err_str(rc);
        return ME_E_IO;
    }
    avio_opened_ = true;
    return ME_OK;
}

me_status_t MuxContext::write_header(std::string* err) {
    if (!fmt_) {
        if (err) *err = "mux_context: no fmt";
        return ME_E_INVALID_ARG;
    }
    if (header_written_) return ME_OK;
    const int rc = avformat_write_header(fmt_, nullptr);
    if (rc < 0) {
        if (err) *err = "write_header: " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    header_written_ = true;
    return ME_OK;
}

me_status_t MuxContext::write_trailer(std::string* err) {
    if (!fmt_) {
        if (err) *err = "mux_context: no fmt";
        return ME_E_INVALID_ARG;
    }
    if (!header_written_) {
        if (err) *err = "mux_context: write_trailer before write_header";
        return ME_E_INVALID_ARG;
    }
    if (trailer_written_) return ME_OK;
    const int rc = av_write_trailer(fmt_);
    if (rc < 0) {
        if (err) *err = "write_trailer: " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    trailer_written_ = true;
    return ME_OK;
}

me_status_t MuxContext::write_and_track(AVPacket*              pkt,
                                         std::vector<int64_t>&  last_end,
                                         std::string*           err) {
    if (!fmt_ || !pkt) {
        if (err) *err = "mux_context: null fmt or pkt";
        if (pkt) av_packet_unref(pkt);
        return ME_E_INVALID_ARG;
    }
    const int si = pkt->stream_index;
    if (si < 0 || si >= static_cast<int>(fmt_->nb_streams)) {
        if (err) *err = "mux_context: pkt.stream_index out of range";
        av_packet_unref(pkt);
        return ME_E_INVALID_ARG;
    }
    if (static_cast<int>(last_end.size()) < static_cast<int>(fmt_->nb_streams)) {
        last_end.resize(fmt_->nb_streams, 0);
    }

    /* Snapshot BEFORE write — libav zeroes pkt's fields on successful
     * ownership transfer. This is the exact footgun that bit the
     * multi-clip concat cycle (see PAIN_POINTS 2026-04-23). */
    const int64_t snapshot =
        (pkt->pts != AV_NOPTS_VALUE)
            ? pkt->pts + std::max<int64_t>(pkt->duration, 0)
            : last_end[si];

    const int rc = av_interleaved_write_frame(fmt_, pkt);
    if (rc == 0) {
        last_end[si] = std::max(last_end[si], snapshot);
    }
    av_packet_unref(pkt);
    if (rc < 0) {
        if (err) *err = "write_frame: " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    return ME_OK;
}

}  // namespace me::io
