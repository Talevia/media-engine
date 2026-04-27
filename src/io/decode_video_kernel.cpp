#include "io/decode_video_kernel.hpp"

#include "graph/types.hpp"
#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "resource/codec_pool.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#include <memory>
#include <span>
#include <string>

namespace me::io {

namespace {

/* Decode forward from the seek position until we hit a frame with
 * pts >= target_pts_stb (in the video stream's time_base). On EOF
 * without reaching target, returns the latest frame we got (target
 * is past stream end). Same hit-or-keep policy that the legacy
 * Previewer used pre-graph-migration. */
me_status_t decode_frame_at(AVFormatContext* fmt,
                            int              video_stream_idx,
                            AVCodecContext*  dec,
                            int64_t          target_pts_stb,
                            AvFramePtr&      out_frame) {
    AvPacketPtr pkt(av_packet_alloc());
    AvFramePtr  fr(av_frame_alloc());

    for (;;) {
        int rc = av_read_frame(fmt, pkt.get());
        if (rc == AVERROR_EOF) {
            rc = avcodec_send_packet(dec, nullptr);
            if (rc < 0 && rc != AVERROR_EOF) return ME_E_DECODE;
            for (;;) {
                rc = avcodec_receive_frame(dec, fr.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) {
                    if (out_frame) return ME_OK;
                    return ME_E_NOT_FOUND;
                }
                if (rc < 0) return ME_E_DECODE;
                out_frame.reset(av_frame_clone(fr.get()));
                av_frame_unref(fr.get());
            }
        }
        if (rc < 0) return ME_E_IO;
        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt.get());
            continue;
        }
        rc = avcodec_send_packet(dec, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0 && rc != AVERROR(EAGAIN)) return ME_E_DECODE;

        for (;;) {
            rc = avcodec_receive_frame(dec, fr.get());
            if (rc == AVERROR(EAGAIN)) break;     /* need more packets */
            if (rc == AVERROR_EOF) {
                if (out_frame) return ME_OK;
                return ME_E_NOT_FOUND;
            }
            if (rc < 0) return ME_E_DECODE;

            /* Hit-or-keep policy: keep latest frame seen and stop once
             * we cross target_pts_stb. If seek lands behind a key
             * frame and we decode a few B/P frames before reaching
             * target, we still return the right one. */
            out_frame.reset(av_frame_clone(fr.get()));
            av_frame_unref(fr.get());
            if (out_frame->pts != AV_NOPTS_VALUE &&
                out_frame->pts >= target_pts_stb) {
                return ME_OK;
            }
        }
    }
}

me_status_t decode_video_kernel(task::TaskContext&                 ctx,
                                const graph::Properties&           props,
                                std::span<const graph::InputValue> inputs,
                                std::span<graph::OutputSlot>       outs) {
    /* Inputs[0] = DemuxContext. Schema-checked by Builder::add. */
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* demux_pp = std::get_if<std::shared_ptr<DemuxContext>>(&inputs[0].v);
    if (!demux_pp || !*demux_pp) return ME_E_INVALID_ARG;
    DemuxContext& demux = **demux_pp;
    if (!demux.fmt) return ME_E_INVALID_ARG;

    /* Required props: source_t_num, source_t_den (both Int64). */
    auto pn = props.find("source_t_num");
    auto pd = props.find("source_t_den");
    if (pn == props.end() || pd == props.end()) return ME_E_INVALID_ARG;
    const auto* num_p = std::get_if<int64_t>(&pn->second.v);
    const auto* den_p = std::get_if<int64_t>(&pd->second.v);
    if (!num_p || !den_p || *den_p <= 0) return ME_E_INVALID_ARG;
    const int64_t source_num = *num_p;
    const int64_t source_den = *den_p;

    AVFormatContext* fmt = demux.fmt;
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) return ME_E_UNSUPPORTED;
    AVStream* vstream = fmt->streams[vs];
    const AVCodec* dec_codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!dec_codec) return ME_E_UNSUPPORTED;

    /* CodecPool may be null in unit-test contexts that build a Scheduler
     * with a stack-only CodecPool — fall back to raw avcodec_alloc_context3
     * so the kernel works without engine wiring. Either path is RAII-clean. */
    resource::CodecPool::Ptr pooled(nullptr, resource::CodecPool::Deleter{nullptr});
    AvCodecContextPtr        owned;
    AVCodecContext*          dec = nullptr;
    if (ctx.codecs) {
        pooled = ctx.codecs->allocate(dec_codec);
        if (!pooled) return ME_E_OUT_OF_MEMORY;
        dec = pooled.get();
    } else {
        owned.reset(avcodec_alloc_context3(dec_codec));
        if (!owned) return ME_E_OUT_OF_MEMORY;
        dec = owned.get();
    }

    int rc = avcodec_parameters_to_context(dec, vstream->codecpar);
    if (rc < 0) return ME_E_INTERNAL;
    dec->pkt_timebase = vstream->time_base;
    rc = avcodec_open2(dec, dec_codec, nullptr);
    if (rc < 0) return ME_E_DECODE;

    /* source_t is in seconds = source_num / source_den. Seek expects
     * AV_TIME_BASE_Q. */
    const int64_t target_us = av_rescale_q(
        source_num,
        AVRational{1, static_cast<int>(source_den)},
        AV_TIME_BASE_Q);
    rc = avformat_seek_file(fmt, -1, INT64_MIN, target_us, target_us,
                             AVSEEK_FLAG_BACKWARD);
    if (rc < 0 && target_us > AV_TIME_BASE) return ME_E_IO;
    avcodec_flush_buffers(dec);

    const int64_t target_pts_stb = av_rescale_q(
        source_num,
        AVRational{1, static_cast<int>(source_den)},
        vstream->time_base);

    AvFramePtr fr;
    me_status_t ds = decode_frame_at(fmt, vs, dec, target_pts_stb, fr);
    if (ds != ME_OK || !fr) return ds;

    /* Hand AVFrame ownership across into a shared_ptr with libav's
     * deleter. unique_ptr → shared_ptr move preserves the deleter. */
    std::shared_ptr<AVFrame> shared_frame(std::move(fr));
    outs[0].v = std::move(shared_frame);
    return ME_OK;
}

}  // namespace

void register_decode_video_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::IoDecodeVideo,
        .affinity       = task::Affinity::Cpu,    /* CPU-bound decode */
        .latency        = task::Latency::Medium,
        .time_invariant = false,                  /* output depends on source_t prop */
        .kernel         = decode_video_kernel,
        .input_schema   = { {"source", graph::TypeId::DemuxCtx} },
        .output_schema  = { {"frame",  graph::TypeId::AvFrameHandle} },
        .param_schema   = {
            {.name = "source_t_num", .type = graph::TypeId::Int64},
            {.name = "source_t_den", .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::io
