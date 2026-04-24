/*
 * frame_puller impl. See frame_puller.hpp for contract.
 */
#include "orchestrator/frame_puller.hpp"

#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "orchestrator/reencode_segment.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#include <climits>

namespace me::orchestrator {

me_status_t pull_next_video_frame(
    AVFormatContext* demux,
    int              video_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err) {

    if (!demux || !dec || !pkt_scratch || !out_frame) {
        if (err) *err = "pull_next_video_frame: null arg";
        return ME_E_INVALID_ARG;
    }
    if (video_stream_idx < 0) {
        if (err) *err = "pull_next_video_frame: negative video_stream_idx";
        return ME_E_INVALID_ARG;
    }

    /* The libav state machine:
     *   receive_frame() can return:
     *     0            — frame ready, done
     *     EAGAIN       — need more packets; send_packet() next
     *     EOF          — flushed, no more frames
     *     <other>      — decode error
     *   send_packet() may be called with a real packet or NULL
     *   (drain mode). Drain is triggered by av_read_frame returning
     *   AVERROR_EOF.
     *
     * Loop: try receive_frame first. On EAGAIN, read one packet
     * from demux (skipping non-video) and send it. On demux EOF,
     * send NULL (drain signal) and loop once more to pick up the
     * flushed frame(s). Non-drain receive_frame EOF → decoder is
     * fully done → return NOT_FOUND. */
    bool draining = false;
    while (true) {
        int rc = avcodec_receive_frame(dec, out_frame);
        if (rc == 0) return ME_OK;
        if (rc == AVERROR(EAGAIN)) {
            /* Need more packet input. */
        } else if (rc == AVERROR_EOF) {
            return ME_E_NOT_FOUND;
        } else {
            if (err) *err = "avcodec_receive_frame: " + me::io::av_err_str(rc);
            return ME_E_DECODE;
        }

        /* receive returned EAGAIN — feed it. */
        if (draining) {
            /* We already sent NULL and got EAGAIN back somehow —
             * libav guarantees the next receive will eventually
             * return EOF in drain mode; keep looping. */
            continue;
        }

        rc = av_read_frame(demux, pkt_scratch);
        if (rc == AVERROR_EOF) {
            /* Send drain signal. */
            draining = true;
            int send_rc = avcodec_send_packet(dec, nullptr);
            if (send_rc < 0) {
                if (err) *err = "avcodec_send_packet(drain): " + me::io::av_err_str(send_rc);
                return ME_E_DECODE;
            }
            continue;
        }
        if (rc < 0) {
            if (err) *err = "av_read_frame: " + me::io::av_err_str(rc);
            return ME_E_IO;
        }

        if (pkt_scratch->stream_index != video_stream_idx) {
            /* Non-video packet — skip, try next. */
            av_packet_unref(pkt_scratch);
            continue;
        }

        int send_rc = avcodec_send_packet(dec, pkt_scratch);
        av_packet_unref(pkt_scratch);
        if (send_rc < 0) {
            if (err) *err = "avcodec_send_packet: " + me::io::av_err_str(send_rc);
            return ME_E_DECODE;
        }
    }
}

me_status_t pull_next_audio_frame(
    AVFormatContext* demux,
    int              audio_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err) {

    /* Structurally identical to pull_next_video_frame — only the
     * packet stream-index filter and the "we want audio" framing
     * differ. libav's codec state machine doesn't distinguish
     * video vs audio at the send_packet/receive_frame API level. */
    if (!demux || !dec || !pkt_scratch || !out_frame) {
        if (err) *err = "pull_next_audio_frame: null arg";
        return ME_E_INVALID_ARG;
    }
    if (audio_stream_idx < 0) {
        if (err) *err = "pull_next_audio_frame: negative audio_stream_idx";
        return ME_E_INVALID_ARG;
    }

    bool draining = false;
    while (true) {
        int rc = avcodec_receive_frame(dec, out_frame);
        if (rc == 0) return ME_OK;
        if (rc == AVERROR(EAGAIN)) {
            /* need more packet input */
        } else if (rc == AVERROR_EOF) {
            return ME_E_NOT_FOUND;
        } else {
            if (err) *err = "avcodec_receive_frame: " + me::io::av_err_str(rc);
            return ME_E_DECODE;
        }

        if (draining) continue;

        rc = av_read_frame(demux, pkt_scratch);
        if (rc == AVERROR_EOF) {
            draining = true;
            int send_rc = avcodec_send_packet(dec, nullptr);
            if (send_rc < 0) {
                if (err) *err = "avcodec_send_packet(drain): " + me::io::av_err_str(send_rc);
                return ME_E_DECODE;
            }
            continue;
        }
        if (rc < 0) {
            if (err) *err = "av_read_frame: " + me::io::av_err_str(rc);
            return ME_E_IO;
        }

        if (pkt_scratch->stream_index != audio_stream_idx) {
            av_packet_unref(pkt_scratch);
            continue;
        }

        int send_rc = avcodec_send_packet(dec, pkt_scratch);
        av_packet_unref(pkt_scratch);
        if (send_rc < 0) {
            if (err) *err = "avcodec_send_packet: " + me::io::av_err_str(send_rc);
            return ME_E_DECODE;
        }
    }
}

me_status_t open_track_decoder(
    std::shared_ptr<me::io::DemuxContext> demux,
    me::resource::CodecPool&               pool,
    TrackDecoderState&                     out,
    std::string*                           err) {

    out = {};   /* zero-init */

    if (!demux || !demux->fmt) {
        if (err) *err = "open_track_decoder: null demux";
        return ME_E_INVALID_ARG;
    }
    AVFormatContext* fmt = demux->fmt;

    /* Find best video stream; negative → no video (acceptable; see
     * header comment). */
    const int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    /* Allocate scratch packet + frame regardless; even "no video"
     * tracks need them for the compose loop's uniform iteration. */
    out.pkt_scratch.reset(av_packet_alloc());
    out.frame_scratch.reset(av_frame_alloc());
    if (!out.pkt_scratch || !out.frame_scratch) {
        if (err) *err = "open_track_decoder: av_packet_alloc/av_frame_alloc";
        return ME_E_OUT_OF_MEMORY;
    }

    if (vsi >= 0) {
        me_status_t s = detail::open_decoder(pool, fmt->streams[vsi], out.dec, err);
        if (s != ME_OK) {
            out = {};
            return s;
        }
    }

    out.demux            = std::move(demux);
    out.video_stream_idx = vsi;
    return ME_OK;
}

me_status_t seek_track_decoder_frame_accurate_to(
    TrackDecoderState& td,
    me_rational_t      target_source_time,
    std::string*       err) {

    if (!td.demux || !td.demux->fmt || !td.dec ||
        !td.pkt_scratch || !td.frame_scratch ||
        td.video_stream_idx < 0) {
        if (err) *err = "seek_track_decoder_frame_accurate_to: unopened td";
        return ME_E_INVALID_ARG;
    }
    AVFormatContext* fmt = td.demux->fmt;
    AVCodecContext*  dec = td.dec.get();

    /* Clamp negatives to zero — seek targets must be ≥ asset start. */
    me_rational_t t = target_source_time;
    if (t.den <= 0) t.den = 1;
    if (t.num < 0)  t.num = 0;

    const int64_t target_us = av_rescale_q(
        t.num,
        AVRational{1, static_cast<int>(t.den)},
        AV_TIME_BASE_Q);

    int rc = avformat_seek_file(fmt, -1, INT64_MIN, target_us, target_us,
                                 AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        /* Small-offset seek failures (near asset start) are tolerable
         * — some demuxers refuse seek but decode-from-start reaches
         * target anyway. Matches the pattern in src/api/thumbnail.cpp.
         * For larger offsets surface the error. */
        if (target_us > AV_TIME_BASE) {
            if (err) *err = "seek_track_decoder_frame_accurate_to: "
                            "avformat_seek_file: " + me::io::av_err_str(rc);
            return ME_E_IO;
        }
    }
    avcodec_flush_buffers(dec);

    AVStream*     st = fmt->streams[td.video_stream_idx];
    const int64_t target_pts_stb = av_rescale_q(
        target_us, AV_TIME_BASE_Q, st->time_base);

    /* Decode forward discarding frames until one with pts >= target
     * lands in `td.frame_scratch`. On EOF during the drop loop, no
     * frame is available at the target and the caller should treat
     * the clip as drained. */
    av_frame_unref(td.frame_scratch.get());
    bool draining = false;
    while (true) {
        int r = avcodec_receive_frame(dec, td.frame_scratch.get());
        if (r == 0) {
            const int64_t pts = (td.frame_scratch->pts != AV_NOPTS_VALUE)
                ? td.frame_scratch->pts
                : td.frame_scratch->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && pts < target_pts_stb) {
                /* Pre-target frame — discard and keep decoding. */
                av_frame_unref(td.frame_scratch.get());
                continue;
            }
            /* At or past target. */
            return ME_OK;
        }
        if (r == AVERROR_EOF) {
            av_frame_unref(td.frame_scratch.get());
            return ME_E_NOT_FOUND;
        }
        if (r != AVERROR(EAGAIN)) {
            if (err) *err = "seek_track_decoder_frame_accurate_to: "
                            "receive_frame: " + me::io::av_err_str(r);
            return ME_E_DECODE;
        }

        if (draining) {
            /* Drain+EAGAIN sequence — libav will eventually return
             * EOF. Keep looping. */
            continue;
        }

        rc = av_read_frame(fmt, td.pkt_scratch.get());
        if (rc == AVERROR_EOF) {
            draining = true;
            int send_rc = avcodec_send_packet(dec, nullptr);
            if (send_rc < 0) {
                if (err) *err = "seek_track_decoder_frame_accurate_to: "
                                "send_packet(drain): " + me::io::av_err_str(send_rc);
                return ME_E_DECODE;
            }
            continue;
        }
        if (rc < 0) {
            if (err) *err = "seek_track_decoder_frame_accurate_to: "
                            "av_read_frame: " + me::io::av_err_str(rc);
            return ME_E_IO;
        }

        if (td.pkt_scratch->stream_index != td.video_stream_idx) {
            av_packet_unref(td.pkt_scratch.get());
            continue;
        }
        int send_rc = avcodec_send_packet(dec, td.pkt_scratch.get());
        av_packet_unref(td.pkt_scratch.get());
        if (send_rc < 0) {
            if (err) *err = "seek_track_decoder_frame_accurate_to: "
                            "send_packet: " + me::io::av_err_str(send_rc);
            return ME_E_DECODE;
        }
    }
}

}  // namespace me::orchestrator
