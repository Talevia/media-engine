#include "orchestrator/reencode_segment.hpp"

#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <memory>
#include <string>

namespace me::orchestrator::detail {

namespace {

using me::io::av_err_str;

using CodecCtxPtr = me::resource::CodecPool::Ptr;
using FramePtr    = me::io::AvFramePtr;
using PacketPtr   = me::io::AvPacketPtr;
using SwsPtr      = me::io::SwsContextPtr;
using SwrPtr      = me::io::SwrContextPtr;

int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
}

bool video_params_compatible(const SharedEncState& s, const AVCodecContext* vdec) {
    return vdec->width == s.v_width && vdec->height == s.v_height &&
           vdec->pix_fmt == s.v_pix;
}

bool audio_params_compatible(const SharedEncState& s, const AVCodecContext* adec) {
    return adec->sample_rate == s.a_sr &&
           adec->sample_fmt  == s.a_fmt &&
           adec->ch_layout.nb_channels == s.a_chans;
}

}  // namespace

me_status_t open_decoder(me::resource::CodecPool&      pool,
                         AVStream*                     in_stream,
                         me::resource::CodecPool::Ptr& out,
                         std::string*                  err) {
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) {
        if (err) *err = std::string("no decoder for ") +
                         avcodec_get_name(in_stream->codecpar->codec_id);
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(dec);
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    int rc = avcodec_parameters_to_context(ctx.get(), in_stream->codecpar);
    if (rc < 0) {
        if (err) *err = "parameters_to_context: " + av_err_str(rc);
        return ME_E_INTERNAL;
    }
    ctx->pkt_timebase = in_stream->time_base;
    rc = avcodec_open2(ctx.get(), dec, nullptr);
    if (rc < 0) {
        if (err) *err = std::string("open decoder ") + dec->name + ": " + av_err_str(rc);
        return ME_E_DECODE;
    }
    out = std::move(ctx);
    return ME_OK;
}

int64_t total_output_us(const std::vector<ReencodeSegment>& segs) {
    int64_t total = 0;
    for (const auto& seg : segs) {
        if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
            total += av_rescale_q(seg.source_duration.num,
                                   AVRational{1, static_cast<int>(seg.source_duration.den)},
                                   AV_TIME_BASE_Q);
        } else if (seg.demux && seg.demux->fmt && seg.demux->fmt->duration > 0) {
            int64_t src_start_us = 0;
            if (seg.source_start.den > 0 && seg.source_start.num > 0) {
                src_start_us = av_rescale_q(seg.source_start.num,
                                             AVRational{1, static_cast<int>(seg.source_start.den)},
                                             AV_TIME_BASE_Q);
            }
            total += std::max<int64_t>(0, seg.demux->fmt->duration - src_start_us);
        }
    }
    return total;
}

me_status_t process_segment(const ReencodeSegment&      seg,
                            me::resource::CodecPool&    pool,
                            SharedEncState&             shared,
                            std::size_t                 seg_idx,
                            std::string*                err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "segment[" + std::to_string(seg_idx) + "] " + std::move(msg);
        return s;
    };

    AVFormatContext* ifmt = seg.demux ? seg.demux->fmt : nullptr;
    if (!ifmt) return fail(ME_E_INVALID_ARG, "no demux context");

    const int vsi = best_stream(ifmt, AVMEDIA_TYPE_VIDEO);
    const int asi = best_stream(ifmt, AVMEDIA_TYPE_AUDIO);

    /* --- Per-segment decoders --- */
    CodecCtxPtr vdec, adec;
    if (vsi >= 0 && shared.venc) {
        me_status_t s = open_decoder(pool, ifmt->streams[vsi], vdec, err);
        if (s != ME_OK) return fail(s, "decoder(video)");
        if (!video_params_compatible(shared, vdec.get())) {
            return fail(ME_E_UNSUPPORTED,
                        "video params (w×h/pix_fmt) differ from segment[0]; "
                        "phase-1: re-encode concat requires identical video params across segments");
        }
    }
    if (asi >= 0 && shared.aenc) {
        me_status_t s = open_decoder(pool, ifmt->streams[asi], adec, err);
        if (s != ME_OK) return fail(s, "decoder(audio)");
        if (!audio_params_compatible(shared, adec.get())) {
            return fail(ME_E_UNSUPPORTED,
                        "audio params (sr/sample_fmt/channels) differ from segment[0]; "
                        "phase-1: re-encode concat requires identical audio params across segments");
        }
    }

    /* --- Per-segment sws/swr. Source pix_fmt/sample_rate are stable per
     *     the compat check above, so sws target is always
     *     shared.venc_pix and swr target is always the shared encoder's
     *     sample_fmt / sample_rate. --- */
    SwsPtr sws;
    FramePtr v_scratch;
    if (vdec && vdec->pix_fmt != shared.venc_pix) {
        sws.reset(sws_getContext(vdec->width, vdec->height, (AVPixelFormat)vdec->pix_fmt,
                                 shared.venc->width, shared.venc->height, shared.venc_pix,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) return fail(ME_E_INTERNAL, "sws_getContext");
        v_scratch.reset(av_frame_alloc());
        v_scratch->format = shared.venc_pix;
        v_scratch->width  = shared.venc->width;
        v_scratch->height = shared.venc->height;
        int rc = av_frame_get_buffer(v_scratch.get(), 32);
        if (rc < 0) return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer(video): " + av_err_str(rc));
    }

    SwrPtr swr;
    if (adec) {
        SwrContext* raw_swr = nullptr;
        int rc = swr_alloc_set_opts2(&raw_swr,
                                     &shared.aenc->ch_layout, shared.aenc->sample_fmt, shared.aenc->sample_rate,
                                     &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
                                     0, nullptr);
        if (rc < 0 || !raw_swr) return fail(ME_E_INTERNAL, "swr_alloc: " + av_err_str(rc));
        swr.reset(raw_swr);
        rc = swr_init(swr.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "swr_init: " + av_err_str(rc));
    }

    /* --- Seek --- */
    int rc = 0;
    const int64_t src_start_us = (seg.source_start.den > 0 && seg.source_start.num > 0)
        ? av_rescale_q(seg.source_start.num,
                        AVRational{1, static_cast<int>(seg.source_start.den)},
                        AV_TIME_BASE_Q)
        : 0;
    if (src_start_us > 0) {
        rc = avformat_seek_file(ifmt, -1, INT64_MIN, src_start_us, src_start_us,
                                 AVSEEK_FLAG_BACKWARD);
        if (rc < 0) return fail(ME_E_IO, "seek: " + av_err_str(rc));
    }
    int64_t src_end_us = INT64_MAX;
    if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
        const int64_t dur_us = av_rescale_q(seg.source_duration.num,
                                             AVRational{1, static_cast<int>(seg.source_duration.den)},
                                             AV_TIME_BASE_Q);
        src_end_us = src_start_us + dur_us;
    }

    /* --- Read/decode/encode loop --- */
    PacketPtr pkt(av_packet_alloc());
    FramePtr  dec_frame(av_frame_alloc());
    me_status_t terminal = ME_OK;

    /* Feed decoded video frames to the encoder, preserving source
     * frame timing (VFR-safe).
     *
     * Historical note: prior to this cycle (pre-vfr-av-sync), the
     * PTS path CFR-forced every frame via `f->pts = next_video_pts;
     * next_video_pts += video_pts_delta`. That flattens VFR input
     * to CFR output, accumulating A/V drift against the audio
     * stream (which is sample-count-driven and therefore tracks
     * real source time). Fix: anchor at the first frame's source
     * PTS, carry source inter-frame intervals through via
     * remap_source_pts_to_output. `segment_base_pts` captures
     * where this segment starts in the cumulative output timeline
     * (= value of shared.next_video_pts on entry), which gives
     * multi-segment concat monotonic output PTS.
     *
     * `shared.next_video_pts` post-fix tracks "next slot beyond
     * everything seen so far" — used by progress reporting + the
     * next segment's segment_base_pts on entry.
     *
     * For CFR input with integer PTS (the deterministic test
     * fixture), the output is byte-identical to the pre-fix CFR
     * path because source PTS spacing already matches
     * video_pts_delta — the test_determinism tripwire guards
     * this.
     *
     * Color pipeline hook: `shared.color_pipeline` (IdentityPipeline
     * today — see ocio-pipeline-wire-first-consumer decision) is
     * invoked on the Y plane before the encoder sees the frame.
     * The src/dst ColorSpace pair is placeholder default-
     * constructed today because the asset-level color space hasn't
     * been threaded through SharedEncState yet. Identity apply
     * preserves bytes so the determinism tripwire stays green. */
    const AVRational vin_tb = (vsi >= 0)
        ? ifmt->streams[vsi]->time_base
        : AVRational{0, 1};
    const int64_t segment_base_pts = shared.next_video_pts;
    int64_t first_src_pts = AV_NOPTS_VALUE;

    auto push_video_frame = [&, vin_tb, segment_base_pts, first_src_pts]
        (AVFrame* f) mutable -> me_status_t {
        if (f) {
            int64_t out_pts = shared.next_video_pts;
            if (f->pts != AV_NOPTS_VALUE) {
                if (first_src_pts == AV_NOPTS_VALUE) {
                    first_src_pts = f->pts;
                }
                out_pts = remap_source_pts_to_output(
                    f->pts, first_src_pts, vin_tb,
                    shared.venc->time_base, segment_base_pts);
            }
            f->pts     = out_pts;
            f->pkt_dts = out_pts;
            /* Advance shared.next_video_pts to track the max output
             * PTS + one delta slot, so progress reporting + next
             * segment's base_pts reflect what's been submitted. */
            const int64_t next_slot = out_pts + shared.video_pts_delta;
            if (next_slot > shared.next_video_pts) {
                shared.next_video_pts = next_slot;
            }

            if (shared.color_pipeline && f->data[0] && f->linesize[0] > 0) {
                const std::size_t y_plane_bytes =
                    static_cast<std::size_t>(f->linesize[0]) *
                    static_cast<std::size_t>(f->height);
                std::string apply_err;
                me_status_t ps = shared.color_pipeline->apply(
                    f->data[0], y_plane_bytes,
                    seg.source_color_space, shared.target_color_space,
                    &apply_err);
                if (ps != ME_OK) {
                    if (err) *err = "color_pipeline.apply: " + std::move(apply_err);
                    return ps;
                }
            }
        }
        return encode_video_frame(f, shared.venc, sws.get(), v_scratch.get(),
                                   shared.ofmt, shared.out_vidx,
                                   shared.venc->time_base, err);
    };

    auto push_audio_frame = [&](AVFrame* f) -> me_status_t {
        return feed_audio_frame(f, swr.get(), adec ? adec->sample_rate : 0,
                                 shared.afifo, shared.aenc, shared.ofmt,
                                 shared.out_aidx, &shared.next_audio_pts, err);
    };

    while (terminal == ME_OK) {
        if (shared.cancel && shared.cancel->load(std::memory_order_acquire)) {
            terminal = ME_E_CANCELLED;
            break;
        }
        rc = av_read_frame(ifmt, pkt.get());
        if (rc == AVERROR_EOF) break;
        if (rc < 0) { terminal = fail(ME_E_DECODE, "read_frame: " + av_err_str(rc)); break; }

        const int si = pkt->stream_index;
        AVCodecContext* dec_ctx = nullptr;
        AVRational in_tb{0, 1};
        bool is_video = false;

        if (si == vsi && vdec) { dec_ctx = vdec.get(); in_tb = ifmt->streams[vsi]->time_base; is_video = true; }
        else if (si == asi && adec) { dec_ctx = adec.get(); in_tb = ifmt->streams[asi]->time_base; }
        else { av_packet_unref(pkt.get()); continue; }

        /* Stop this segment once past source_duration (compare in source tb). */
        if (pkt->pts != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pkt->pts, in_tb, AV_TIME_BASE_Q);
            if (pts_us >= src_end_us) {
                av_packet_unref(pkt.get());
                break;
            }
        }

        rc = avcodec_send_packet(dec_ctx, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) { terminal = fail(ME_E_DECODE, "send_packet: " + av_err_str(rc)); break; }

        while (true) {
            rc = avcodec_receive_frame(dec_ctx, dec_frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { terminal = fail(ME_E_DECODE, "receive_frame: " + av_err_str(rc)); break; }

            me_status_t s = is_video ? push_video_frame(dec_frame.get())
                                      : push_audio_frame(dec_frame.get());
            av_frame_unref(dec_frame.get());
            if (s != ME_OK) { terminal = s; break; }

            /* Progress — next_video_pts is monotonic across all
             * segments (single shared encoder), so rescaling it to
             * AV_TIME_BASE_Q gives the cumulative output duration
             * directly without a separate per-segment accumulator. */
            if (is_video && shared.on_ratio && shared.total_us > 0) {
                const int64_t cum_us = av_rescale_q(shared.next_video_pts,
                                                     shared.venc->time_base,
                                                     AV_TIME_BASE_Q);
                float ratio = static_cast<float>(cum_us) / static_cast<float>(shared.total_us);
                if (ratio < 0.f) ratio = 0.f;
                if (ratio > 1.f) ratio = 1.f;
                shared.on_ratio(ratio);
            }
        }
        if (terminal != ME_OK) break;
    }

    /* --- End-of-segment: flush per-segment decoders into the shared
     *     encoder. Do NOT flush the shared encoder here — encoder flush
     *     only happens once, after all segments, inside reencode_mux. --- */
    if (terminal == ME_OK && vdec) {
        rc = avcodec_send_packet(vdec.get(), nullptr);
        if (rc >= 0) {
            while (terminal == ME_OK) {
                rc = avcodec_receive_frame(vdec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = fail(ME_E_DECODE, "flush decode(video): " + av_err_str(rc)); break; }
                me_status_t s = push_video_frame(dec_frame.get());
                av_frame_unref(dec_frame.get());
                if (s != ME_OK) { terminal = s; break; }
            }
        }
    }
    if (terminal == ME_OK && adec) {
        rc = avcodec_send_packet(adec.get(), nullptr);
        if (rc >= 0) {
            while (terminal == ME_OK) {
                rc = avcodec_receive_frame(adec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = fail(ME_E_DECODE, "flush decode(audio): " + av_err_str(rc)); break; }
                me_status_t s = push_audio_frame(dec_frame.get());
                av_frame_unref(dec_frame.get());
                if (s != ME_OK) { terminal = s; break; }
            }
        }
        /* Flush residual swr state (no more input samples) and push
         * converted leftovers into the FIFO. */
        if (terminal == ME_OK) {
            me_status_t s = push_audio_frame(nullptr);
            if (s != ME_OK) terminal = s;
        }
    }

    return terminal;
}

}  // namespace me::orchestrator::detail
