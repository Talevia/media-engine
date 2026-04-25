/*
 * Detail helpers extracted from reencode_segment.cpp.
 *
 * `open_decoder` and `total_output_us` are declared in
 * reencode_segment.hpp and consumed widely (reencode_pipeline,
 * frame_puller, encoder_mux_setup, plus reencode_segment.cpp's
 * own process_segment). Living next to process_segment in the
 * same TU pushed reencode_segment.cpp to 375 lines (within §1a's
 * 400-line warning band); splitting them out drops the host file
 * to ~315 lines and keeps either of the two cross-consumer
 * helpers in their own focused TU.
 *
 * No behavior change — these are byte-for-byte the same impls
 * that lived in reencode_segment.cpp pre-split. Public surface
 * (the declarations in reencode_segment.hpp) is unchanged.
 */
#include "orchestrator/reencode_segment.hpp"

#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <string>
#include <utility>

namespace me::orchestrator::detail {

me_status_t open_decoder(me::resource::CodecPool&      pool,
                         AVStream*                     in_stream,
                         me::resource::CodecPool::Ptr& out,
                         std::string*                  err) {
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) {
        if (err) *err = std::string("no decoder for ") +
                         avcodec_get_name(in_stream->codecpar->codec_id);
        /* LEGIT: FFmpeg build missing a decoder for the input stream's
         * codec (rare on stock builds; surfaced clearly). */
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(dec);
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    int rc = avcodec_parameters_to_context(ctx.get(), in_stream->codecpar);
    if (rc < 0) {
        if (err) *err = "parameters_to_context: " + me::io::av_err_str(rc);
        return ME_E_INTERNAL;
    }
    ctx->pkt_timebase = in_stream->time_base;
    rc = avcodec_open2(ctx.get(), dec, nullptr);
    if (rc < 0) {
        if (err) *err = std::string("open decoder ") + dec->name + ": " +
                         me::io::av_err_str(rc);
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

}  // namespace me::orchestrator::detail
