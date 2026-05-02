/*
 * HevcSwSink impl. See header for the dispatch contract + scope
 * limits. The flow per `process()`:
 *
 *   demux→decoder→swscale (to YUV420P 8-bit if needed)
 *     → KvazaarHevcEncoder.send_frame
 *     → flush_packets → AVPacket via av_new_packet (no aliasing —
 *       Kvazaar reuses its byte buffer across calls)
 *     → av_interleaved_write_frame into the raw "hevc" muxer.
 *
 * The raw-hevc muxer accepts Annex-B input directly; the parameter
 * sets (VPS/SPS/PPS) flow inline as NAL units in the first packet,
 * which is how Annex-B HEVC is transported on disk. `.hevc` files
 * are playable in ffplay / VLC; MP4 wrapping requires hvcC
 * extradata extraction, deferred to the follow-up bullet.
 */
#include "orchestrator/hevc_sw_sink.hpp"

#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/codec_resolver.hpp"

#ifdef ME_HAS_KVAZAAR
#include "io/kvazaar_hevc_encoder.hpp"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator {

namespace {

using me::io::av_err_str;
using me::io::AvCodecContextPtr;
using me::io::AvFramePtr;
using me::io::AvPacketPtr;
using me::io::SwsContextPtr;

}  // namespace

bool is_hevc_sw_video_only_spec(const me_output_spec_t& spec) {
    /* Routes through the typed-codec resolver (cycle-47 ABI):
     * `(hevc-sw, none)` means video=HEVC_SW + audio=NONE in the
     * resolved enum, regardless of whether the host populated
     * the legacy strings or the typed `codec_options` extension.
     * The resolver maps NULL / "" / "none" → ME_AUDIO_CODEC_NONE
     * exactly as the prior string-based check did. */
    const CodecSelection sel = resolve_codec_selection(spec);
    return sel.video_codec == ME_VIDEO_CODEC_HEVC_SW
        && sel.audio_codec == ME_AUDIO_CODEC_NONE;
}

#ifndef ME_HAS_KVAZAAR

class HevcSwSinkUnsupported final : public OutputSink {
public:
    HevcSwSinkUnsupported() = default;
    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>>,
        std::string* err) override {
        if (err) *err = "hevc-sw sink: engine built without ME_WITH_KVAZAAR=ON; "
                        "rebuild with -DME_WITH_KVAZAAR=ON + brew install kvazaar "
                        "(or apt install libkvazaar-dev)";
        /* LEGIT: build-flag-gated stub class — only compiled when
         * ME_HAS_KVAZAAR is undefined. The rejection IS the
         * documented behavior for OFF builds; the real impl
         * (the `#else` branch above) handles ON builds. */
        return ME_E_UNSUPPORTED;
    }
};

std::unique_ptr<OutputSink> make_hevc_sw_sink(
    SinkCommon /*common*/,
    std::vector<ClipTimeRange> /*clip_ranges*/) {
    return std::make_unique<HevcSwSinkUnsupported>();
}

#else  /* ME_HAS_KVAZAAR */

namespace {

int best_video_stream(AVFormatContext* fmt) {
    return av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
}

}  // namespace

class HevcSwSink final : public OutputSink {
public:
    HevcSwSink(SinkCommon common, std::vector<ClipTimeRange> ranges)
        : common_(std::move(common)), ranges_(std::move(ranges)) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {

        auto fail = [&](me_status_t s, std::string msg) {
            if (err) *err = "hevc-sw sink: " + std::move(msg);
            return s;
        };

        if (demuxes.size() != 1 || ranges_.size() != 1) {
            return fail(ME_E_UNSUPPORTED,
                        "single-segment only (multi-segment SW HEVC is "
                        "the debt-hevc-sw-multi-segment follow-up)");
        }
        auto demux = demuxes[0];
        if (!demux) return fail(ME_E_INVALID_ARG, "null demux context");
        AVFormatContext* ifmt = demux->fmt;
        if (!ifmt) return fail(ME_E_INVALID_ARG, "demux has no fmt");

        const int vsi = best_video_stream(ifmt);
        if (vsi < 0) return fail(ME_E_INVALID_ARG, "input has no video stream");

        /* --- Open decoder ---------------------------------------- */
        AVStream* in_vstream = ifmt->streams[vsi];
        const AVCodec* dec = avcodec_find_decoder(in_vstream->codecpar->codec_id);
        if (!dec) return fail(ME_E_UNSUPPORTED, "no decoder for input video codec");
        AvCodecContextPtr dec_ctx(avcodec_alloc_context3(dec));
        if (!dec_ctx) return fail(ME_E_OUT_OF_MEMORY, "decoder ctx alloc");
        int rc = avcodec_parameters_to_context(dec_ctx.get(), in_vstream->codecpar);
        if (rc < 0) return fail(ME_E_INTERNAL, "parameters_to_context: " + av_err_str(rc));
        rc = avcodec_open2(dec_ctx.get(), dec, nullptr);
        if (rc < 0) return fail(ME_E_INTERNAL, "open decoder: " + av_err_str(rc));

        const int width  = dec_ctx->width;
        const int height = dec_ctx->height;
        if (width <= 0 || height <= 0) {
            return fail(ME_E_INVALID_ARG, "decoder returned non-positive dimensions");
        }

        /* --- Open Kvazaar encoder -------------------------------- */
        AVRational fr_av = av_guess_frame_rate(ifmt, in_vstream, nullptr);
        if (fr_av.num <= 0 || fr_av.den <= 0) fr_av = AVRational{30, 1};
        std::string kvz_err;
        auto kvz = me::io::KvazaarHevcEncoder::create(
            width, height, fr_av.num, fr_av.den, /*bitrate_bps=*/0, &kvz_err);
        if (!kvz) return fail(ME_E_UNSUPPORTED, "KvazaarHevcEncoder: " + kvz_err);

        /* --- Open output mux (raw Annex-B "hevc") ---------------- */
        std::string mux_err;
        auto mux = me::io::MuxContext::open(common_.out_path, "hevc", &mux_err);
        if (!mux) return fail(ME_E_UNSUPPORTED, "mux open: " + mux_err);
        AVFormatContext* ofmt = mux->fmt();

        AVStream* out_vstream = avformat_new_stream(ofmt, nullptr);
        if (!out_vstream) return fail(ME_E_OUT_OF_MEMORY, "new_stream(video)");
        out_vstream->codecpar->codec_type      = AVMEDIA_TYPE_VIDEO;
        out_vstream->codecpar->codec_id        = AV_CODEC_ID_HEVC;
        out_vstream->codecpar->width           = width;
        out_vstream->codecpar->height          = height;
        out_vstream->codecpar->format          = AV_PIX_FMT_YUV420P;
        out_vstream->codecpar->color_range     = in_vstream->codecpar->color_range;
        out_vstream->codecpar->color_primaries = in_vstream->codecpar->color_primaries;
        out_vstream->codecpar->color_trc       = in_vstream->codecpar->color_trc;
        out_vstream->codecpar->color_space     = in_vstream->codecpar->color_space;
        /* Microsecond time_base — the output PTS we stamp into each
         * packet is in microseconds (AV_TIME_BASE_Q-equivalent),
         * matching `pts_us - src_start_us` from the decode loop. The
         * raw-hevc muxer rescales internally if needed. */
        out_vstream->time_base                 = AVRational{1, AV_TIME_BASE};
        out_vstream->avg_frame_rate            = fr_av;
        out_vstream->r_frame_rate              = fr_av;

        if (auto s = mux->open_avio(err);    s != ME_OK) return s;
        if (auto s = mux->write_header(err); s != ME_OK) return s;

        /* --- swscale to YUV420P 8-bit if input is anything else -- */
        SwsContextPtr sws;
        AvFramePtr scratch_yuv;
        const AVPixelFormat in_pixfmt = static_cast<AVPixelFormat>(dec_ctx->pix_fmt);
        if (in_pixfmt != AV_PIX_FMT_YUV420P) {
            sws.reset(sws_getContext(width, height, in_pixfmt,
                                      width, height, AV_PIX_FMT_YUV420P,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws) return fail(ME_E_INTERNAL, "sws_getContext");
            scratch_yuv.reset(av_frame_alloc());
            if (!scratch_yuv) return fail(ME_E_OUT_OF_MEMORY, "frame_alloc(scratch)");
            scratch_yuv->format = AV_PIX_FMT_YUV420P;
            scratch_yuv->width  = width;
            scratch_yuv->height = height;
            rc = av_frame_get_buffer(scratch_yuv.get(), 32);
            if (rc < 0) return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer: " + av_err_str(rc));
        }

        /* --- Seek to source_start, derive source_end -------------- */
        const ClipTimeRange& range = ranges_[0];
        const int64_t src_start_us = (range.source_start.den > 0 && range.source_start.num >= 0)
            ? av_rescale_q(range.source_start.num,
                            AVRational{1, static_cast<int>(range.source_start.den)},
                            AV_TIME_BASE_Q)
            : 0;
        int64_t src_end_us = INT64_MAX;
        if (range.source_duration.den > 0 && range.source_duration.num > 0) {
            const int64_t dur_us = av_rescale_q(range.source_duration.num,
                                                 AVRational{1, static_cast<int>(range.source_duration.den)},
                                                 AV_TIME_BASE_Q);
            src_end_us = src_start_us + dur_us;
        }
        if (src_start_us > 0) {
            rc = avformat_seek_file(ifmt, -1, INT64_MIN, src_start_us, src_start_us,
                                     AVSEEK_FLAG_BACKWARD);
            if (rc < 0) return fail(ME_E_IO, "seek: " + av_err_str(rc));
        }

        /* --- AVPacket emitter for Kvazaar bytes ------------------ */
        /* Kvazaar reuses its byte buffer across calls — we cannot
         * alias. Copy bytes via av_new_packet (libav-managed
         * allocation) and write through the muxer. Errors are
         * captured via `flush_err` since `flush_packets`'s callback
         * has a void return. */
        std::string flush_err;
        bool        flush_failed = false;
        int64_t     next_out_pts = 0;
        const int64_t pts_per_frame_us =
            (fr_av.num > 0) ? av_rescale_q(1, AVRational{fr_av.den, fr_av.num},
                                            AV_TIME_BASE_Q) : 33333;

        auto write_chunk = [&](std::span<const std::uint8_t> bytes) {
            if (flush_failed) return;
            if (bytes.empty()) return;
            AvPacketPtr out_pkt(av_packet_alloc());
            if (!out_pkt) {
                flush_err = "av_packet_alloc OOM";
                flush_failed = true;
                return;
            }
            int rc2 = av_new_packet(out_pkt.get(), static_cast<int>(bytes.size()));
            if (rc2 < 0) {
                flush_err = "av_new_packet: " + av_err_str(rc2);
                flush_failed = true;
                return;
            }
            std::memcpy(out_pkt->data, bytes.data(), bytes.size());
            out_pkt->pts          = next_out_pts;
            out_pkt->dts          = next_out_pts;
            out_pkt->stream_index = out_vstream->index;
            /* Each Kvazaar `flush_packets` call delivers all chunks
             * for a single encoded frame; the raw-hevc muxer is fine
             * with treating every emitted packet as a keyframe-ish
             * boundary since parameter sets accompany IDRs in-band. */
            out_pkt->flags |= AV_PKT_FLAG_KEY;
            int rc3 = av_interleaved_write_frame(ofmt, out_pkt.get());
            if (rc3 < 0) {
                flush_err = "write_frame: " + av_err_str(rc3);
                flush_failed = true;
                return;
            }
            next_out_pts += pts_per_frame_us;
        };

        /* --- Decode/encode loop ---------------------------------- */
        AvPacketPtr pkt(av_packet_alloc());
        AvFramePtr  dec_frame(av_frame_alloc());
        if (!pkt || !dec_frame) return fail(ME_E_OUT_OF_MEMORY, "pkt/frame alloc");
        if (common_.on_ratio) common_.on_ratio(0.0f);

        bool reached_eof = false;
        bool done        = false;
        while (!done) {
            if (common_.cancel && common_.cancel->load()) {
                return fail(ME_E_CANCELLED, "cancel");
            }

            if (!reached_eof) {
                rc = av_read_frame(ifmt, pkt.get());
                if (rc == AVERROR_EOF) {
                    rc = avcodec_send_packet(dec_ctx.get(), nullptr);
                    if (rc < 0 && rc != AVERROR_EOF) {
                        return fail(ME_E_DECODE, "send_packet(EOF): " + av_err_str(rc));
                    }
                    reached_eof = true;
                } else if (rc < 0) {
                    return fail(ME_E_IO, "read_frame: " + av_err_str(rc));
                } else {
                    if (pkt->stream_index != vsi) {
                        av_packet_unref(pkt.get());
                        continue;
                    }
                    rc = avcodec_send_packet(dec_ctx.get(), pkt.get());
                    av_packet_unref(pkt.get());
                    if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
                        return fail(ME_E_DECODE, "send_packet: " + av_err_str(rc));
                    }
                }
            }

            while (true) {
                rc = avcodec_receive_frame(dec_ctx.get(), dec_frame.get());
                if (rc == AVERROR(EAGAIN)) break;
                if (rc == AVERROR_EOF) { done = true; break; }
                if (rc < 0) return fail(ME_E_DECODE, "receive_frame: " + av_err_str(rc));

                /* Range filter on source-stream PTS in microseconds. */
                const int64_t pts_us = (dec_frame->pts != AV_NOPTS_VALUE)
                    ? av_rescale_q(dec_frame->pts, in_vstream->time_base, AV_TIME_BASE_Q)
                    : 0;
                if (pts_us < src_start_us) {
                    av_frame_unref(dec_frame.get());
                    continue;
                }
                if (pts_us >= src_end_us) {
                    av_frame_unref(dec_frame.get());
                    done = true;
                    break;
                }

                /* Convert to YUV420P 8-bit if needed. */
                const std::uint8_t* y; std::size_t sy;
                const std::uint8_t* u; std::size_t su;
                const std::uint8_t* v; std::size_t sv;
                if (sws) {
                    rc = sws_scale(sws.get(), dec_frame->data, dec_frame->linesize,
                                    0, dec_frame->height,
                                    scratch_yuv->data, scratch_yuv->linesize);
                    if (rc < 0) return fail(ME_E_INTERNAL, "sws_scale: " + av_err_str(rc));
                    y = scratch_yuv->data[0]; sy = static_cast<std::size_t>(scratch_yuv->linesize[0]);
                    u = scratch_yuv->data[1]; su = static_cast<std::size_t>(scratch_yuv->linesize[1]);
                    v = scratch_yuv->data[2]; sv = static_cast<std::size_t>(scratch_yuv->linesize[2]);
                } else {
                    y = dec_frame->data[0]; sy = static_cast<std::size_t>(dec_frame->linesize[0]);
                    u = dec_frame->data[1]; su = static_cast<std::size_t>(dec_frame->linesize[1]);
                    v = dec_frame->data[2]; sv = static_cast<std::size_t>(dec_frame->linesize[2]);
                }

                std::string kerr;
                me_status_t ks = kvz->send_frame(y, sy, u, su, v, sv,
                                                  pts_us - src_start_us, &kerr);
                if (ks != ME_OK) return fail(ks, "kvz send_frame: " + kerr);

                me_status_t fs = kvz->flush_packets(write_chunk, &kerr);
                if (fs != ME_OK) return fail(fs, "kvz flush_packets: " + kerr);
                if (flush_failed) return fail(ME_E_ENCODE, flush_err);

                if (common_.on_ratio && src_end_us != INT64_MAX) {
                    const int64_t total_us = src_end_us - src_start_us;
                    if (total_us > 0) {
                        const float r = static_cast<float>(pts_us - src_start_us) /
                                         static_cast<float>(total_us);
                        common_.on_ratio(r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r));
                    }
                }
                av_frame_unref(dec_frame.get());
            }
        }

        /* --- Drain Kvazaar reorder buffer ------------------------ */
        kvz->send_eof();
        std::string kerr;
        me_status_t fs = kvz->flush_packets(write_chunk, &kerr);
        if (fs != ME_OK) return fail(fs, "kvz drain: " + kerr);
        if (flush_failed) return fail(ME_E_ENCODE, flush_err);

        if (auto s = mux->write_trailer(err); s != ME_OK) return s;
        if (common_.on_ratio) common_.on_ratio(1.0f);
        return ME_OK;
    }

private:
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
};

std::unique_ptr<OutputSink> make_hevc_sw_sink(
    SinkCommon                 common,
    std::vector<ClipTimeRange> clip_ranges) {
    return std::make_unique<HevcSwSink>(std::move(common), std::move(clip_ranges));
}

#endif  /* ME_HAS_KVAZAAR */

}  // namespace me::orchestrator
