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
}

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

}  // namespace me::orchestrator
