#include "io/decode_audio_kernel.hpp"

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

/* Decode audio forward until we see a frame whose pts is >= target.
 * Mirror IoDecodeVideo's hit-or-keep policy: keep the latest decoded
 * frame so callers seeking past EOF still get the closest preceding
 * frame. */
me_status_t decode_audio_at(AVFormatContext* fmt,
                             int              audio_stream_idx,
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
        if (pkt->stream_index != audio_stream_idx) {
            av_packet_unref(pkt.get());
            continue;
        }
        rc = avcodec_send_packet(dec, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0 && rc != AVERROR(EAGAIN)) return ME_E_DECODE;

        for (;;) {
            rc = avcodec_receive_frame(dec, fr.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) {
                if (out_frame) return ME_OK;
                return ME_E_NOT_FOUND;
            }
            if (rc < 0) return ME_E_DECODE;

            out_frame.reset(av_frame_clone(fr.get()));
            av_frame_unref(fr.get());
            if (out_frame->pts != AV_NOPTS_VALUE &&
                out_frame->pts >= target_pts_stb) {
                return ME_OK;
            }
        }
    }
}

me_status_t decode_audio_kernel(task::TaskContext&                 ctx,
                                 const graph::Properties&           props,
                                 std::span<const graph::InputValue> inputs,
                                 std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* demux_pp = std::get_if<std::shared_ptr<DemuxContext>>(&inputs[0].v);
    if (!demux_pp || !*demux_pp) return ME_E_INVALID_ARG;
    DemuxContext& demux = **demux_pp;
    if (!demux.fmt) return ME_E_INVALID_ARG;

    auto pn = props.find("source_t_num");
    auto pd = props.find("source_t_den");
    if (pn == props.end() || pd == props.end()) return ME_E_INVALID_ARG;
    const auto* num_p = std::get_if<int64_t>(&pn->second.v);
    const auto* den_p = std::get_if<int64_t>(&pd->second.v);
    if (!num_p || !den_p || *den_p <= 0) return ME_E_INVALID_ARG;
    const int64_t source_num = *num_p;
    const int64_t source_den = *den_p;

    AVFormatContext* fmt = demux.fmt;
    const int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (as < 0) return ME_E_UNSUPPORTED;
    AVStream* astream = fmt->streams[as];
    const AVCodec* dec_codec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!dec_codec) return ME_E_UNSUPPORTED;

    /* CodecPool reuse pattern matches IoDecodeVideo — null pool is
     * acceptable (test contexts construct stack-only). */
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

    int rc = avcodec_parameters_to_context(dec, astream->codecpar);
    if (rc < 0) return ME_E_INTERNAL;
    dec->pkt_timebase = astream->time_base;
    rc = avcodec_open2(dec, dec_codec, nullptr);
    if (rc < 0) return ME_E_DECODE;

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
        astream->time_base);

    AvFramePtr fr;
    me_status_t ds = decode_audio_at(fmt, as, dec, target_pts_stb, fr);
    if (ds != ME_OK || !fr) return ds;

    std::shared_ptr<AVFrame> shared_frame(std::move(fr));
    outs[0].v = std::move(shared_frame);
    return ME_OK;
}

}  // namespace

void register_decode_audio_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::IoDecodeAudio,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Medium,
        .time_invariant = false,
        .kernel         = decode_audio_kernel,
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
