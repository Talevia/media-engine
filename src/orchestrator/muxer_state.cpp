#include "orchestrator/muxer_state.hpp"

#include "io/demux_context.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

me_status_t passthrough_mux(io::DemuxContext&            demux,
                             const PassthroughMuxOptions& opts,
                             std::string*                 err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    AVFormatContext* ifmt = demux.fmt;
    if (!ifmt) return fail(ME_E_INVALID_ARG, "demux context has no AVFormatContext");

    AVFormatContext* ofmt = nullptr;
    const char* format_name = opts.container.empty() ? nullptr : opts.container.c_str();
    int rc = avformat_alloc_output_context2(&ofmt, nullptr, format_name, opts.out_path.c_str());
    if (rc < 0 || !ofmt) return fail(ME_E_INTERNAL, "alloc output: " + av_err_str(rc));

    std::vector<int> stream_map(ifmt->nb_streams, -1);
    for (unsigned i = 0; i < ifmt->nb_streams; ++i) {
        AVStream* in_s = ifmt->streams[i];
        const AVCodecParameters* par = in_s->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) {
            avformat_free_context(ofmt);
            return fail(ME_E_OUT_OF_MEMORY, "new_stream");
        }
        rc = avcodec_parameters_copy(out_s->codecpar, par);
        if (rc < 0) {
            avformat_free_context(ofmt);
            return fail(ME_E_INTERNAL, "codecpar_copy: " + av_err_str(rc));
        }
        out_s->codecpar->codec_tag = 0;
        stream_map[i] = out_s->index;
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&ofmt->pb, opts.out_path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            avformat_free_context(ofmt);
            return fail(ME_E_IO, "avio_open: " + av_err_str(rc));
        }
    }

    rc = avformat_write_header(ofmt, nullptr);
    if (rc < 0) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
        return fail(ME_E_ENCODE, "write_header: " + av_err_str(rc));
    }

    if (opts.on_ratio) opts.on_ratio(0.0f);
    const int64_t total_duration = (ifmt->duration > 0) ? ifmt->duration : 0;

    AVPacket* pkt = av_packet_alloc();
    me_status_t terminal = ME_OK;

    while ((rc = av_read_frame(ifmt, pkt)) >= 0) {
        if (opts.cancel && opts.cancel->load(std::memory_order_acquire)) {
            terminal = ME_E_CANCELLED;
            av_packet_unref(pkt);
            break;
        }
        int mapped = (pkt->stream_index < (int)stream_map.size())
                         ? stream_map[pkt->stream_index] : -1;
        if (mapped < 0) { av_packet_unref(pkt); continue; }

        AVStream* in_s  = ifmt->streams[pkt->stream_index];
        AVStream* out_s = ofmt->streams[mapped];
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->stream_index = mapped;
        pkt->pos = -1;

        const int64_t pts_for_progress = pkt->pts;
        const AVRational out_tb = out_s->time_base;

        rc = av_interleaved_write_frame(ofmt, pkt);
        av_packet_unref(pkt);
        if (rc < 0) {
            terminal = ME_E_ENCODE;
            if (err) *err = "write_frame: " + av_err_str(rc);
            break;
        }

        if (opts.on_ratio && total_duration > 0 && pts_for_progress != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pts_for_progress, out_tb, AV_TIME_BASE_Q);
            float ratio = (float)pts_us / (float)total_duration;
            if (ratio < 0.f) ratio = 0.f;
            if (ratio > 1.f) ratio = 1.f;
            opts.on_ratio(ratio);
        }
    }
    av_packet_free(&pkt);

    if (terminal == ME_OK && rc < 0 && rc != AVERROR_EOF) {
        terminal = ME_E_DECODE;
        if (err) *err = "read_frame: " + av_err_str(rc);
    }

    if (terminal == ME_OK) {
        rc = av_write_trailer(ofmt);
        if (rc < 0) {
            terminal = ME_E_ENCODE;
            if (err) *err = "write_trailer: " + av_err_str(rc);
        }
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
