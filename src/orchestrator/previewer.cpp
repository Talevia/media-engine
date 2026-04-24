#include "orchestrator/previewer.hpp"

#include "compose/frame_convert.hpp"
#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <memory>
#include <string>

namespace me::orchestrator {

namespace {

std::string strip_file_scheme(const std::string& uri) {
    const std::string prefix = "file://";
    if (uri.size() > prefix.size() && uri.substr(0, prefix.size()) == prefix) {
        return uri.substr(prefix.size());
    }
    return uri;
}

/* Decode forward from the seek position until we hit a frame with
 * pts ≥ target_pts_stb. On EOF without reaching target, returns
 * the latest frame we got (target is past stream end). */
me_status_t decode_frame_at(AVFormatContext*             fmt,
                             int                          video_stream_idx,
                             AVCodecContext*              dec,
                             int64_t                      target_pts_stb,
                             me::io::AvFramePtr&          out_frame,
                             std::string*                 err) {
    me::io::AvPacketPtr pkt(av_packet_alloc());
    me::io::AvFramePtr  fr(av_frame_alloc());

    for (;;) {
        int rc = av_read_frame(fmt, pkt.get());
        if (rc == AVERROR_EOF) {
            rc = avcodec_send_packet(dec, nullptr);
            if (rc < 0 && rc != AVERROR_EOF) {
                if (err) *err = "send flush: " + me::io::av_err_str(rc);
                return ME_E_DECODE;
            }
            for (;;) {
                rc = avcodec_receive_frame(dec, fr.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) {
                    if (out_frame) return ME_OK;
                    if (err) *err = "drained without producing a frame";
                    return ME_E_NOT_FOUND;
                }
                if (rc < 0) {
                    if (err) *err = "receive(flush): " + me::io::av_err_str(rc);
                    return ME_E_DECODE;
                }
                out_frame.reset(av_frame_clone(fr.get()));
                av_frame_unref(fr.get());
            }
        }
        if (rc < 0) {
            if (err) *err = "read_frame: " + me::io::av_err_str(rc);
            return ME_E_IO;
        }
        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt.get());
            continue;
        }
        rc = avcodec_send_packet(dec, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            if (err) *err = "send_packet: " + me::io::av_err_str(rc);
            return ME_E_DECODE;
        }
        for (;;) {
            rc = avcodec_receive_frame(dec, fr.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) {
                if (out_frame) return ME_OK;
                if (err) *err = "EOF before target";
                return ME_E_NOT_FOUND;
            }
            if (rc < 0) {
                if (err) *err = "receive_frame: " + me::io::av_err_str(rc);
                return ME_E_DECODE;
            }
            const int64_t frame_pts =
                (fr->pts != AV_NOPTS_VALUE) ? fr->pts : fr->best_effort_timestamp;
            if (frame_pts != AV_NOPTS_VALUE && frame_pts < target_pts_stb) {
                out_frame.reset(av_frame_clone(fr.get()));
                av_frame_unref(fr.get());
                continue;
            }
            out_frame.reset(av_frame_clone(fr.get()));
            av_frame_unref(fr.get());
            return ME_OK;
        }
    }
}

}  // namespace

me_status_t Previewer::frame_at(me_rational_t time, me_frame** out_frame) {
    if (!out_frame) return ME_E_INVALID_ARG;
    *out_frame = nullptr;
    if (!tl_) return ME_E_INVALID_ARG;
    if (tl_->tracks.empty() || tl_->clips.empty()) return ME_E_INVALID_ARG;

    /* Normalize input time. */
    if (time.den <= 0) time.den = 1;
    if (time.num < 0)  time.num = 0;

    /* Find the bottom track's active clip at t. Phase-1 single-
     * track frame server — multi-track compose via the full
     * compose_decode_loop path is a follow-up cycle. */
    const std::string& bottom_id = tl_->tracks[0].id;
    const me::Clip*   active = nullptr;
    me_rational_t     clip_local_t{0, 1};

    for (const auto& c : tl_->clips) {
        if (c.track_id != bottom_id) continue;

        /* time_start + time_duration in rational form: sum. */
        const int64_t e_num = c.time_start.num * c.time_duration.den +
                              c.time_duration.num * c.time_start.den;
        const int64_t e_den = c.time_start.den * c.time_duration.den;

        /* t >= start AND t < end, cross-multiplied. */
        const bool ge_start =
            time.num * c.time_start.den >= c.time_start.num * time.den;
        const bool lt_end =
            time.num * e_den < e_num * time.den;
        if (!ge_start || !lt_end) continue;

        active = &c;
        /* source_t = source_start + (t - time_start). The "t -
         * time_start" part is rational subtract: a/b - c/d =
         * (ad - cb)/(bd). */
        clip_local_t = me_rational_t{
            time.num * c.time_start.den - c.time_start.num * time.den,
            time.den * c.time_start.den,
        };
        break;
    }
    if (!active) return ME_E_NOT_FOUND;

    /* source_t_abs = source_start + clip_local_t. */
    const me_rational_t source_t{
        active->source_start.num * clip_local_t.den +
            clip_local_t.num * active->source_start.den,
        active->source_start.den * clip_local_t.den,
    };

    /* Asset lookup. */
    auto a_it = tl_->assets.find(active->asset_id);
    if (a_it == tl_->assets.end()) return ME_E_NOT_FOUND;
    const std::string path = strip_file_scheme(a_it->second.uri);

    /* Open input + find video stream + spin up decoder. */
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) return ME_E_IO;
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) { avformat_close_input(&fmt); return ME_E_DECODE; }

    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fmt); return ME_E_UNSUPPORTED; }
    AVStream* vstream = fmt->streams[vs];
    const AVCodec* dec_codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!dec_codec) { avformat_close_input(&fmt); return ME_E_UNSUPPORTED; }

    auto dec = (engine_ && engine_->codecs)
        ? engine_->codecs->allocate(dec_codec)
        : me::resource::CodecPool::Ptr(nullptr, me::resource::CodecPool::Deleter{nullptr});
    if (!dec) { avformat_close_input(&fmt); return ME_E_OUT_OF_MEMORY; }

    rc = avcodec_parameters_to_context(dec.get(), vstream->codecpar);
    if (rc < 0) { avformat_close_input(&fmt); return ME_E_INTERNAL; }
    dec->pkt_timebase = vstream->time_base;
    rc = avcodec_open2(dec.get(), dec_codec, nullptr);
    if (rc < 0) { avformat_close_input(&fmt); return ME_E_DECODE; }

    /* Seek to source_t in AV_TIME_BASE_Q units. */
    const int64_t target_us = av_rescale_q(
        source_t.num,
        AVRational{1, static_cast<int>(source_t.den)},
        AV_TIME_BASE_Q);
    rc = avformat_seek_file(fmt, -1, INT64_MIN, target_us, target_us,
                             AVSEEK_FLAG_BACKWARD);
    if (rc < 0 && target_us > AV_TIME_BASE) {
        avformat_close_input(&fmt);
        return ME_E_IO;
    }
    avcodec_flush_buffers(dec.get());

    const int64_t target_pts_stb = av_rescale_q(
        source_t.num,
        AVRational{1, static_cast<int>(source_t.den)},
        vstream->time_base);

    me::io::AvFramePtr fr;
    std::string err;
    const me_status_t ds = decode_frame_at(fmt, vs, dec.get(),
                                            target_pts_stb, fr, &err);
    if (ds != ME_OK || !fr) {
        avformat_close_input(&fmt);
        return ds;
    }

    /* Convert to tightly-packed RGBA8. */
    auto mf = std::make_unique<me_frame>();
    mf->width  = fr->width;
    mf->height = fr->height;
    mf->stride = mf->width * 4;

    const me_status_t cs = me::compose::frame_to_rgba8(fr.get(), mf->rgba, &err);
    avformat_close_input(&fmt);
    if (cs != ME_OK) return cs;

    *out_frame = mf.release();
    return ME_OK;
}

}  // namespace me::orchestrator
