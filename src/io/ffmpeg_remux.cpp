#include "io/ffmpeg_remux.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <string>
#include <vector>

namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/* Strip a leading "file://" scheme; return the rest unchanged. */
std::string strip_file_scheme(const std::string& uri) {
    constexpr std::string_view prefix{"file://"};
    if (uri.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), uri.begin())) {
        return uri.substr(prefix.size());
    }
    return uri;
}

}  // namespace

namespace me::io {

me_status_t remux_passthrough(
    const std::string& in_uri,
    const std::string& out_path,
    const std::string& container_hint,
    std::function<void(float)> on_progress,
    const std::atomic<bool>& cancel,
    std::string* err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    const std::string in_path = strip_file_scheme(in_uri);

    AVFormatContext* ifmt = nullptr;
    AVFormatContext* ofmt = nullptr;
    int ret = 0;

    ret = avformat_open_input(&ifmt, in_path.c_str(), nullptr, nullptr);
    if (ret < 0) return fail(ME_E_IO, "open input: " + av_err_str(ret));

    ret = avformat_find_stream_info(ifmt, nullptr);
    if (ret < 0) {
        avformat_close_input(&ifmt);
        return fail(ME_E_DECODE, "find_stream_info: " + av_err_str(ret));
    }

    const char* format_name = container_hint.empty() ? nullptr : container_hint.c_str();
    ret = avformat_alloc_output_context2(&ofmt, nullptr, format_name, out_path.c_str());
    if (ret < 0 || !ofmt) {
        avformat_close_input(&ifmt);
        return fail(ME_E_INTERNAL, "alloc output: " + av_err_str(ret));
    }

    std::vector<int> stream_map(ifmt->nb_streams, -1);
    for (unsigned i = 0; i < ifmt->nb_streams; i++) {
        AVStream* in_stream = ifmt->streams[i];
        const AVCodecParameters* par = in_stream->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* out_stream = avformat_new_stream(ofmt, nullptr);
        if (!out_stream) {
            avformat_free_context(ofmt);
            avformat_close_input(&ifmt);
            return fail(ME_E_OUT_OF_MEMORY, "new_stream");
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, par);
        if (ret < 0) {
            avformat_free_context(ofmt);
            avformat_close_input(&ifmt);
            return fail(ME_E_INTERNAL, "codecpar_copy: " + av_err_str(ret));
        }
        out_stream->codecpar->codec_tag = 0;
        stream_map[i] = out_stream->index;
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt->pb, out_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            avformat_free_context(ofmt);
            avformat_close_input(&ifmt);
            return fail(ME_E_IO, "avio_open: " + av_err_str(ret));
        }
    }

    ret = avformat_write_header(ofmt, nullptr);
    if (ret < 0) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
        avformat_close_input(&ifmt);
        return fail(ME_E_ENCODE, "write_header: " + av_err_str(ret));
    }

    if (on_progress) on_progress(0.0f);

    const int64_t total_duration = (ifmt->duration > 0) ? ifmt->duration : 0;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        av_write_trailer(ofmt);
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
        avformat_close_input(&ifmt);
        return fail(ME_E_OUT_OF_MEMORY, "packet alloc");
    }

    me_status_t terminal = ME_OK;
    while ((ret = av_read_frame(ifmt, pkt)) >= 0) {
        if (cancel.load(std::memory_order_acquire)) {
            terminal = ME_E_CANCELLED;
            av_packet_unref(pkt);
            break;
        }
        int mapped = (pkt->stream_index < (int)stream_map.size()) ?
                     stream_map[pkt->stream_index] : -1;
        if (mapped < 0) { av_packet_unref(pkt); continue; }

        AVStream* in_stream  = ifmt->streams[pkt->stream_index];
        AVStream* out_stream = ofmt->streams[mapped];
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = mapped;
        pkt->pos = -1;

        /* Capture pts before write_frame / unref potentially reset the packet. */
        const int64_t pts_for_progress = pkt->pts;
        const AVRational out_tb = out_stream->time_base;

        ret = av_interleaved_write_frame(ofmt, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            terminal = ME_E_ENCODE;
            if (err) *err = "write_frame: " + av_err_str(ret);
            break;
        }

        if (on_progress && total_duration > 0 && pts_for_progress != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pts_for_progress, out_tb, AV_TIME_BASE_Q);
            float ratio = (float)pts_us / (float)total_duration;
            if (ratio < 0.f) ratio = 0.f;
            if (ratio > 1.f) ratio = 1.f;
            on_progress(ratio);
        }
    }
    av_packet_free(&pkt);

    if (terminal == ME_OK && ret < 0 && ret != AVERROR_EOF) {
        terminal = ME_E_DECODE;
        if (err) *err = "read_frame: " + av_err_str(ret);
    }

    if (terminal == ME_OK) {
        ret = av_write_trailer(ofmt);
        if (ret < 0) {
            terminal = ME_E_ENCODE;
            if (err) *err = "write_trailer: " + av_err_str(ret);
        }
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);
    avformat_close_input(&ifmt);

    if (terminal == ME_OK && on_progress) on_progress(1.0f);
    return terminal;
}

}  // namespace me::io
