#include "orchestrator/reencode_pipeline.hpp"

#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/encoder_mux_setup.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_segment.hpp"
#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
}

#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

using me::io::av_err_str;

using CodecCtxPtr = me::resource::CodecPool::Ptr;

using detail::drain_audio_fifo;
using detail::encode_audio_frame;
using detail::encode_video_frame;
using detail::open_audio_encoder;
using detail::open_decoder;
using detail::open_video_encoder;
using detail::process_segment;
using detail::SharedEncState;
using detail::total_output_us;

}  // namespace

me_status_t reencode_mux(const ReencodeOptions& opts,
                         std::string*           err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    /* Cycle-49 typed-codec migration: branch on
     * `video_codec_enum` / `audio_codec_enum` (populated by
     * `make_output_sink` from `resolve_codec_selection`). The
     * string fields remain the diagnostic source for the
     * "unsupported codec '<name>'" error message so a host that
     * misnames a codec sees what it actually wrote, not the
     * resolver's NONE coercion. */
    switch (opts.video_codec_enum) {
    case ME_VIDEO_CODEC_H264:
    case ME_VIDEO_CODEC_HEVC:
    case ME_VIDEO_CODEC_HEVC_SW:
        break;  /* accepted */
    case ME_VIDEO_CODEC_NONE:
    case ME_VIDEO_CODEC_PASSTHROUGH:
        return fail(ME_E_UNSUPPORTED,
                    "video_codec=\"" + opts.video_codec +
                    "\" not supported (expected \"h264\", \"hevc\", or \"hevc-sw\")");
    }
    if (opts.audio_codec_enum != ME_AUDIO_CODEC_AAC) {
        return fail(ME_E_UNSUPPORTED,
                    "audio_codec=\"" + opts.audio_codec + "\" not supported (expected \"aac\")");
    }
    if (!opts.pool) return fail(ME_E_INVALID_ARG, "reencode_mux: opts.pool is required");
    if (opts.segments.empty()) return fail(ME_E_INVALID_ARG, "reencode_mux: segments is empty");

    AVFormatContext* ifmt0 = opts.segments.front().demux ? opts.segments.front().demux->fmt : nullptr;
    if (!ifmt0) return fail(ME_E_INVALID_ARG, "segment[0] has no demux context");

    /* Encoder + mux bootstrap extracted to `encoder_mux_setup.cpp` so
     * future multi-source sinks (ComposeSink frame loop, cross-
     * dissolve sink, audio-mix scheduler) can reuse the same plumbing
     * rather than copy-pasting the decoder-sniff + open_*_encoder +
     * avformat_new_stream + afifo_alloc chain. Behavior unchanged. */
    std::unique_ptr<me::io::MuxContext> mux;
    CodecCtxPtr venc, aenc;
    SharedEncState shared;
    if (auto s = setup_h264_aac_encoder_mux(opts, ifmt0, mux, venc, aenc, shared, err);
        s != ME_OK) {
        return s;
    }
    AVFormatContext* ofmt = mux->fmt();

    /* FIFO must be freed explicitly; wrap cleanup so any early-return
     * path still releases it. */
    struct FifoGuard {
        AVAudioFifo* f;
        ~FifoGuard() { if (f) av_audio_fifo_free(f); }
    } fifo_guard{shared.afifo};

    if (auto s = mux->open_avio(err);    s != ME_OK) return s;
    if (auto s = mux->write_header(err); s != ME_OK) return s;

    if (opts.on_ratio) opts.on_ratio(0.0f);
    me_status_t terminal = ME_OK;

    for (std::size_t i = 0; i < opts.segments.size() && terminal == ME_OK; ++i) {
        terminal = process_segment(opts.segments[i], *opts.pool, shared, i, err);
    }

    /* --- Flush the shared encoders once, after all segments. --- */
    if (terminal == ME_OK && shared.venc) {
        me_status_t s = encode_video_frame(nullptr, shared.venc, nullptr, nullptr,
                                           ofmt, shared.out_vidx, shared.venc->time_base, err);
        if (s != ME_OK) terminal = s;
    }
    if (terminal == ME_OK && shared.aenc) {
        me_status_t s = drain_audio_fifo(shared.afifo, shared.aenc, ofmt,
                                          shared.out_aidx, &shared.next_audio_pts,
                                          /*flush=*/true, err);
        if (s != ME_OK) terminal = s;
        if (terminal == ME_OK) {
            s = encode_audio_frame(nullptr, shared.aenc, ofmt, shared.out_aidx, err);
            if (s != ME_OK) terminal = s;
        }
    }

    if (terminal == ME_OK) {
        if (auto s = mux->write_trailer(err); s != ME_OK) terminal = s;
    }

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
