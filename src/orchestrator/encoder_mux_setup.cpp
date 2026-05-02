/*
 * encoder_mux_setup impl. Factored verbatim from the top half of
 * reencode_pipeline::reencode_mux (lines ~65–158 of the pre-refactor
 * file). No behavior change.
 */
#include "orchestrator/encoder_mux_setup.hpp"

#include "color/pipeline.hpp"
#include "io/av_err.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_video.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
}

namespace me::orchestrator {

using CodecCtxPtr = me::resource::CodecPool::Ptr;

namespace {

int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
}

int64_t total_output_us_local(const std::vector<ReencodeSegment>& segs) {
    /* Mirror of `detail::total_output_us` — wanted to avoid exposing
     * that implementation-detail helper across TU boundaries, so we
     * re-compute here from the same ReencodeSegment fields. */
    int64_t us = 0;
    for (const auto& s : segs) {
        if (s.source_duration.den <= 0) continue;
        us += av_rescale_q(s.source_duration.num,
                           AVRational{1, static_cast<int>(s.source_duration.den)},
                           AV_TIME_BASE_Q);
    }
    return us;
}

}  // namespace

me_status_t setup_h264_aac_encoder_mux(
    const ReencodeOptions&                 opts,
    AVFormatContext*                       sample_demux,
    std::unique_ptr<me::io::MuxContext>&   out_mux,
    me::resource::CodecPool::Ptr&          out_venc,
    me::resource::CodecPool::Ptr&          out_aenc,
    detail::SharedEncState&                out_shared,
    std::string*                           err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    if (!opts.pool) return fail(ME_E_INVALID_ARG, "setup_h264_aac_encoder_mux: opts.pool required");
    if (!sample_demux) return fail(ME_E_INVALID_ARG, "setup_h264_aac_encoder_mux: sample_demux null");

    AVFormatContext* ifmt0 = sample_demux;
    /* When opts.audio_only is true, the sample demux's video stream
     * (if any) is ignored: the output mux gets only an audio stream
     * and no video encoder is opened. This is how AudioOnlySink
     * reuses this setup helper with a sample demux that happens to
     * contain video (e.g. an audio clip referencing a file that
     * also has a video track). */
    const int vsi0 = opts.audio_only ? -1
                                      : best_stream(ifmt0, AVMEDIA_TYPE_VIDEO);
    const int asi0 = best_stream(ifmt0, AVMEDIA_TYPE_AUDIO);
    if (vsi0 < 0 && asi0 < 0) {
        return fail(ME_E_INVALID_ARG,
                    opts.audio_only
                        ? "sample_demux has no audio (audio_only requested)"
                        : "sample_demux has neither video nor audio");
    }

    /* Open sample decoders for parameter inference. They're reset at
     * the end of this function — only the encoder state outlives. */
    CodecCtxPtr v0dec, a0dec;
    if (vsi0 >= 0) {
        me_status_t s = detail::open_decoder(*opts.pool, ifmt0->streams[vsi0], v0dec, err);
        if (s != ME_OK) return s;
    }
    if (asi0 >= 0) {
        me_status_t s = detail::open_decoder(*opts.pool, ifmt0->streams[asi0], a0dec, err);
        if (s != ME_OK) return s;
    }

    std::string open_err;
    auto mux = me::io::MuxContext::open(opts.out_path, opts.container, &open_err);
    if (!mux) return fail(ME_E_UNSUPPORTED, std::move(open_err));
    AVFormatContext* ofmt = mux->fmt();

    out_shared = {};   /* zero-init */
    out_shared.ofmt           = ofmt;
    out_shared.cancel         = opts.cancel;
    out_shared.on_ratio       = opts.on_ratio;
    out_shared.total_us       = total_output_us_local(opts.segments);
    out_shared.color_pipeline = me::color::make_pipeline(
        opts.ocio_config_path.empty() ? nullptr : opts.ocio_config_path.c_str());
    out_shared.target_color_space = opts.target_color_space;

    CodecCtxPtr venc, aenc;
    int rc = 0;
    if (v0dec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(video)");
        out_s->time_base = ifmt0->streams[vsi0]->time_base;

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = detail::open_video_encoder(*opts.pool, v0dec.get(),
                                                    ifmt0->streams[vsi0]->time_base,
                                                    opts.video_bitrate_bps, global_header,
                                                    opts.video_codec_enum,
                                                    opts.video_codec,
                                                    venc, out_shared.venc_pix, err);
        if (s != ME_OK) return s;

        rc = avcodec_parameters_from_context(out_s->codecpar, venc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(video): " + me::io::av_err_str(rc));

        /* Propagate HDR static metadata side data from the source
         * stream's codecpar into the output stream's codecpar.
         * `avcodec_parameters_from_context` (above) initialises
         * `out_s->codecpar->coded_side_data` from the encoder's
         * side data, which doesn't carry the source's mdcv / clli
         * unless we explicitly attached it — for HEVC re-encode
         * (cycle 11) we did NOT attach those, so the moov atom
         * would lose `mdcv` / `clli` boxes vs. the input. Mirrors
         * the side-data attachment that
         * `tests/fixtures/gen_hdr_fixture.cpp` does on a fresh
         * write; here we copy from the source side data instead.
         *
         * Limited to MASTERING_DISPLAY_METADATA + CONTENT_LIGHT_LEVEL
         * — those are the two HDR static-metadata kinds the
         * `me_hdr_static_metadata_t` C API surfaces. Other side-data
         * kinds (display matrix, stereo3d) propagate via the
         * existing av_packet_rescale_ts / packet copy paths and
         * are out of scope. */
        if (vsi0 >= 0) {
            const AVCodecParameters* src_cp = ifmt0->streams[vsi0]->codecpar;
            for (int i = 0; i < src_cp->nb_coded_side_data; ++i) {
                const AVPacketSideData& sd = src_cp->coded_side_data[i];
                if (sd.type != AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
                    sd.type != AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
                    continue;
                }
                AVPacketSideData* new_sd = av_packet_side_data_new(
                    &out_s->codecpar->coded_side_data,
                    &out_s->codecpar->nb_coded_side_data,
                    sd.type, sd.size, 0);
                if (!new_sd) return fail(ME_E_OUT_OF_MEMORY,
                    "av_packet_side_data_new(HDR re-encode)");
                std::memcpy(new_sd->data, sd.data, sd.size);
            }
        }

        out_s->avg_frame_rate = v0dec->framerate;
        out_s->r_frame_rate   = v0dec->framerate;
        out_shared.venc      = venc.get();
        out_shared.out_vidx  = out_s->index;
        out_shared.v_width   = v0dec->width;
        out_shared.v_height  = v0dec->height;
        out_shared.v_pix     = (AVPixelFormat)v0dec->pix_fmt;

        AVRational fr = av_guess_frame_rate(ifmt0, ifmt0->streams[vsi0], nullptr);
        if (fr.num <= 0 || fr.den <= 0) fr = AVRational{25, 1};
        out_shared.video_pts_delta = av_rescale_q(1, av_inv_q(fr), venc->time_base);
        if (out_shared.video_pts_delta <= 0) out_shared.video_pts_delta = 1;
    }
    if (a0dec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(audio)");

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = detail::open_audio_encoder(*opts.pool, a0dec.get(),
                                                    opts.audio_bitrate_bps,
                                                    global_header, aenc, err);
        if (s != ME_OK) return s;
        out_s->time_base = aenc->time_base;

        rc = avcodec_parameters_from_context(out_s->codecpar, aenc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(audio): " + me::io::av_err_str(rc));
        out_shared.aenc     = aenc.get();
        out_shared.out_aidx = out_s->index;
        out_shared.a_sr     = a0dec->sample_rate;
        out_shared.a_fmt    = a0dec->sample_fmt;
        out_shared.a_chans  = a0dec->ch_layout.nb_channels;

        out_shared.afifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, 1);
        if (!out_shared.afifo) return fail(ME_E_OUT_OF_MEMORY, "audio_fifo_alloc");
    }

    /* Release sample decoders; encoder state is what matters going forward. */
    v0dec.reset();
    a0dec.reset();

    /* Hand ownership to the caller. */
    out_mux  = std::move(mux);
    out_venc = std::move(venc);
    out_aenc = std::move(aenc);
    return ME_OK;
}

}  // namespace me::orchestrator
