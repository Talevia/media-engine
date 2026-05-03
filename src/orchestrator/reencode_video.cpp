#include "orchestrator/reencode_video.hpp"

#include "io/av_err.hpp"
#include "orchestrator/codec_descriptor_table.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <string>

namespace me::orchestrator::detail {

namespace {

using me::io::av_err_str;
using PacketPtr = me::io::AvPacketPtr;

}  // namespace

me_status_t open_video_encoder(me::resource::CodecPool&      pool,
                               const AVCodecContext*         dec,
                               AVRational                    stream_time_base,
                               int64_t                       bitrate_bps,
                               bool                          global_header,
                               me_video_codec_t              video_codec_enum,
                               const std::string&            video_codec,
                               me::resource::CodecPool::Ptr& out_enc,
                               AVPixelFormat&                out_target_pix,
                               std::string*                  err) {
    /* Cycle-49 typed-codec dispatch (M7-debt
     * debt-reencode-pipeline-internal-strcmp-migration). The
     * caller resolves the spec via `resolve_codec_selection`
     * and threads the enum here; the legacy string parameter is
     * preserved for the diagnostic message + the SW-HEVC
     * branch's unique error wording.
     *
     * Dispatch consults the unified codec_descriptor_table
     * (debt-codec-dispatch-table-unification): one source of
     * truth maps the enum to (encoder name, pix_fmt, default
     * bitrate). Adding a new codec is one descriptor entry, not
     * separate edits in codec_resolver.cpp + here. */

    /* NONE + non-empty string = unknown codec name (resolver
     * coerces strings it doesn't know into NONE). Reject with
     * the original string in the diagnostic — preserves the
     * legacy `unsupported video_codec '<name>'` error wording. */
    if (video_codec_enum == ME_VIDEO_CODEC_NONE && !video_codec.empty()) {
        if (err) *err = "open_video_encoder: unsupported video_codec '" +
                        video_codec + "' (expected '' / 'h264' / 'hevc' / 'hevc-sw')";
        /* LEGIT: codec-enum dispatch is closed over the set we
         * ship; unknown names land here for explicit rejection. */
        return ME_E_UNSUPPORTED;
    }

    /* Legacy: NONE (empty input string) falls back to H264 — the
     * pre-cycle-49 default. Audio-only paths skip this function
     * entirely so the fallback was effectively unreachable in
     * production, but the defensive coercion stays cheap. */
    me_video_codec_t effective_enum = video_codec_enum;
    if (effective_enum == ME_VIDEO_CODEC_NONE) {
        effective_enum = ME_VIDEO_CODEC_H264;
    }

    /* PASSTHROUGH should be dispatched at make_output_sink to
     * the PassthroughSink; reaching this re-encode path with
     * PASSTHROUGH is a caller bug. */
    if (effective_enum == ME_VIDEO_CODEC_PASSTHROUGH) {
        if (err) *err = "open_video_encoder: unsupported video_codec '" +
                        video_codec + "' (passthrough should not reach the "
                        "re-encode path; expected '' / 'h264' / 'hevc' / 'hevc-sw')";
        /* LEGIT: PASSTHROUGH is dispatched at make_output_sink to
         * a separate sink class; reaching here is a caller bug. */
        return ME_E_UNSUPPORTED;
    }

    const VideoCodecDescriptor* desc = lookup_video_codec_by_enum(effective_enum);
    if (!desc || !desc->avcodec_encoder_name) {
        if (err) *err = "open_video_encoder: unsupported video_codec '" +
                        video_codec + "' (expected '' / 'h264' / 'hevc' / 'hevc-sw')";
        /* LEGIT: codec-enum dispatch is closed over the set we
         * ship. Future codec additions land via new descriptor
         * entries; until then UNSUPPORTED is the right shape. */
        return ME_E_UNSUPPORTED;
    }

    const char*   enc_name        = desc->avcodec_encoder_name;
    AVPixelFormat enc_pix         = desc->pix_fmt;
    int64_t       default_bitrate = desc->default_bitrate_bps;
    const bool    is_hevc         = (effective_enum == ME_VIDEO_CODEC_HEVC);
    const bool    is_hevc_sw      = (effective_enum == ME_VIDEO_CODEC_HEVC_SW);

    /* SW HEVC fallback dispatch (Kvazaar, BSD-3, LGPL-clean per VISION
     * §3.4). M10 exit criterion 3 requires a SW path when VideoToolbox
     * is unavailable (Linux / Windows hosts) or when the caller wants
     * cross-host bit-identity comparison. The encode-loop itself —
     * pumping YUV420P planes through `KvazaarHevcEncoder` and emitting
     * Annex-B chunks via `av_packet_from_data` into the libavformat mux —
     * is tracked by `encode-hevc-sw-encode-loop-impl` in BACKLOG. This
     * branch performs the preflight checks (link-time ME_HAS_KVAZAAR,
     * 1080p ceiling, multiple-of-8 alignment — mirroring
     * KvazaarHevcEncoder::create's runtime checks) and returns
     * ME_E_UNSUPPORTED with a diagnostic that names the BACKLOG item
     * so callers see a stable error point regardless of how far the
     * pipeline progresses. */
    if (is_hevc_sw) {
#ifndef ME_HAS_KVAZAAR
        if (err) *err = "open_video_encoder: 'hevc-sw' requires ME_WITH_KVAZAAR=ON "
                        "+ pkg-config kvazaar (brew install kvazaar / "
                        "apt install libkvazaar-dev); this engine build was "
                        "compiled without Kvazaar";
        /* LEGIT: build-flag-gated rejection — this path is the
         * intentional surface when the engine is compiled without
         * Kvazaar; the diag tells the host how to enable it. */
        return ME_E_UNSUPPORTED;
#else
        if (dec->width <= 0 || dec->height <= 0) {
            if (err) *err = "open_video_encoder: 'hevc-sw' requires positive "
                            "width/height (got " + std::to_string(dec->width) +
                            "x" + std::to_string(dec->height) + ")";
            return ME_E_INVALID_ARG;
        }
        if (dec->width > 1920 || dec->height > 1080) {
            if (err) *err = "open_video_encoder: 'hevc-sw' SW HEVC ceiling is "
                            "1920x1080 (got " + std::to_string(dec->width) +
                            "x" + std::to_string(dec->height) + "); use "
                            "video_codec='hevc' for HW VideoToolbox path "
                            "(see docs/MILESTONES.md M10 §3)";
            /* LEGIT: SW HEVC has a 1080p resolution ceiling per
             * M10 §3 ("limited output"); larger inputs MUST go
             * through the HW path. The rejection IS the documented
             * outcome — not a stub. */
            return ME_E_UNSUPPORTED;
        }
        if ((dec->width & 7) || (dec->height & 7)) {
            if (err) *err = "open_video_encoder: 'hevc-sw' width/height must be "
                            "multiples of 8 (HEVC CTU alignment; got " +
                            std::to_string(dec->width) + "x" +
                            std::to_string(dec->height) + ")";
            return ME_E_INVALID_ARG;
        }
        if (err) *err = "open_video_encoder: 'hevc-sw' encode-loop wiring "
                        "pending for the AAC-paired spec (use audio_codec="
                        "'none' to route through HevcSwSink which produces "
                        "raw Annex-B HEVC end-to-end)";
        /* LEGIT: (hevc-sw, aac) spec — MP4+AAC mux integration is
         * downstream work; (hevc-sw, none) spec already lands on
         * the working HevcSwSink. */
        return ME_E_UNSUPPORTED;
#endif
    }

    const AVCodec* enc = avcodec_find_encoder_by_name(enc_name);
    if (!enc) {
        if (err) {
            *err = std::string{"encoder "} + enc_name +
                   " not available in this FFmpeg build";
            if (is_hevc) {
                /* HW probe miss on the HEVC path is the explicit cue to
                 * try the LGPL-clean SW fallback. Spelling out
                 * `video_codec='hevc-sw'` keeps Linux / Windows hosts
                 * from chasing the VideoToolbox dependency. */
                *err += "; on hosts without VideoToolbox try "
                        "video_codec='hevc-sw' (LGPL-clean Kvazaar SW "
                        "fallback, requires ME_WITH_KVAZAAR=ON, 1080p "
                        "ceiling, marked non-deterministic per "
                        "docs/MILESTONES.md M10 §3)";
            }
        }
        /* LEGIT: Linux / Windows hosts lack videotoolbox; reject with
         * a clear diagnostic so callers know the platform gate. */
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(enc);
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    ctx->width               = dec->width;
    ctx->height              = dec->height;
    ctx->pix_fmt             = enc_pix;
    ctx->time_base           = stream_time_base;
    ctx->framerate           = dec->framerate;
    ctx->sample_aspect_ratio = dec->sample_aspect_ratio;
    /* Color tags propagate verbatim from the decoder. For HDR sources
     * tagged BT.2020 + PQ the resulting `color_primaries` /
     * `color_trc` / `colorspace` cause `hevc_videotoolbox` to emit ST
     * 2086 mastering-display + CTA-861.3 content-light SEI from the
     * codec's defaults. Custom mastering-display values (host-supplied
     * MaxCLL / MaxFALL / chromaticities) flow via packet side-data in
     * a future cycle (`test-hdr-metadata-propagate`). */
    ctx->color_range         = dec->color_range;
    ctx->color_primaries     = dec->color_primaries;
    ctx->color_trc           = dec->color_trc;
    ctx->colorspace          = dec->colorspace;
    ctx->bit_rate            = (bitrate_bps > 0) ? bitrate_bps
                                                  : default_bitrate;
    /* MP4 / MOV need extradata carried in the container's 'avcC' / 'hvcC'
     * box, not prefixed to keyframes — MUST be set before avcodec_open2. */
    if (global_header) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* Hint to the encoder / its helpers to avoid baking libav version
     * strings into extradata or packets. Paired with AVFMT_FLAG_BITEXACT
     * on the muxer — see test_determinism / decision
     * 2026-04-23-debt-render-bitexact-flags. videotoolbox is HW so
     * this flag is advisory; it's still worth passing for the software-
     * path intent and for other encoders future re-encode paths may
     * pick up. */
    ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    int rc = avcodec_open2(ctx.get(), enc, nullptr);
    if (rc < 0) {
        if (err) *err = std::string{"open "} + enc_name + ": " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    out_enc = std::move(ctx);
    out_target_pix = enc_pix;
    return ME_OK;
}

me_status_t encode_video_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               SwsContext*      sws,
                               AVFrame*         scratch_nv12,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               AVRational       in_stream_tb,
                               std::string*     err) {
    AVFrame* to_encode = in_frame;

    if (in_frame && sws) {
        int rc = sws_scale(sws,
                           in_frame->data, in_frame->linesize,
                           0, in_frame->height,
                           scratch_nv12->data, scratch_nv12->linesize);
        if (rc < 0) {
            if (err) *err = "sws_scale: " + av_err_str(rc);
            return ME_E_INTERNAL;
        }
        scratch_nv12->pts     = av_rescale_q(in_frame->pts,     in_stream_tb, enc->time_base);
        scratch_nv12->pkt_dts = av_rescale_q(in_frame->pkt_dts, in_stream_tb, enc->time_base);
        to_encode = scratch_nv12;
    } else if (in_frame) {
        in_frame->pts = av_rescale_q(in_frame->pts, in_stream_tb, enc->time_base);
    }

    int rc = avcodec_send_frame(enc, to_encode);
    if (rc < 0 && rc != AVERROR_EOF) {
        if (err) *err = "send_frame(video): " + av_err_str(rc);
        return ME_E_ENCODE;
    }

    PacketPtr out_pkt(av_packet_alloc());
    while (true) {
        rc = avcodec_receive_packet(enc, out_pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (err) *err = "receive_packet(video): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
        out_pkt->stream_index = out_stream_idx;
        av_packet_rescale_ts(out_pkt.get(), enc->time_base,
                              ofmt->streams[out_stream_idx]->time_base);
        rc = av_interleaved_write_frame(ofmt, out_pkt.get());
        av_packet_unref(out_pkt.get());
        if (rc < 0) {
            if (err) *err = "write_frame(video): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
    }
    return ME_OK;
}

int64_t remap_source_pts_to_output(int64_t     src_pts,
                                    int64_t     first_src_pts,
                                    AVRational  src_tb,
                                    AVRational  out_tb,
                                    int64_t     segment_base_out_pts) {
    const int64_t src_offset     = src_pts - first_src_pts;
    const int64_t out_offset     = av_rescale_q(src_offset, src_tb, out_tb);
    return segment_base_out_pts + out_offset;
}

}  // namespace me::orchestrator::detail
