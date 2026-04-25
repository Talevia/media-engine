/*
 * ComposeSink class — per-output-frame multi-track composite via
 * alpha_over. Extracted from compose_sink.cpp (which kept the
 * public is_gpu_compose_usable predicate + make_compose_sink
 * validation factory) to keep both files comfortably under the
 * §1a 400-line ceiling.
 *
 * Wires together the six prereqs that prior scope-A cycles landed:
 *   - setup_h264_aac_encoder_mux (encoder + mux bootstrap)
 *   - open_track_decoder / TrackDecoderState (per-track decode bundle)
 *   - run_compose_video_frame_loop (per-frame compose loop, in
 *     compose_decode_loop.cpp)
 *   - me::compose::active_clips_at (resolve which clip is active per
 *     track at timeline time T)
 *   - me::compose::frame_to_rgba8 / rgba8_to_frame (AVFrame ↔ RGBA8)
 *   - me::compose::alpha_over (Porter-Duff src-over, Normal blend)
 *
 * Phase-1 simplifications: see compose_sink.cpp's preamble.
 */
#include "orchestrator/compose_sink_impl.hpp"

#include "audio/mixer.hpp"
#include "orchestrator/compose_audio.hpp"
#include "orchestrator/compose_decode_loop.hpp"
#include "gpu/gpu_backend.hpp"
#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/encoder_mux_setup.hpp"
#include "orchestrator/frame_puller.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_pipeline.hpp"
#include "orchestrator/reencode_segment.hpp"
#include "orchestrator/reencode_video.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

#include <utility>

namespace me::orchestrator {

namespace {

class ComposeSink final : public OutputSink {
public:
    ComposeSink(const me::Timeline&        tl,
                SinkCommon                 common,
                std::vector<ClipTimeRange> ranges,
                me::resource::CodecPool*   pool,
                const me::gpu::GpuBackend* gpu,
                int64_t                    video_bitrate,
                int64_t                    audio_bitrate)
        : tl_(tl),
          common_(std::move(common)),
          ranges_(std::move(ranges)),
          pool_(pool),
          gpu_backend_(gpu),
          video_bitrate_(video_bitrate),
          audio_bitrate_(audio_bitrate) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        if (demuxes.size() != ranges_.size()) {
            if (err) *err = "ComposeSink: demuxes / ranges size mismatch";
            return ME_E_INTERNAL;
        }
        if (tl_.tracks.empty() || tl_.clips.empty()) {
            if (err) *err = "ComposeSink: empty timeline";
            return ME_E_INVALID_ARG;
        }

        /* Build a minimal ReencodeOptions for the setup helper +
         * passthrough audio. Audio stream params come from demuxes[0]
         * (tracks[0]'s first clip), matching the single-track reencode
         * convention. `segments` is used by setup_h264_aac_encoder_mux
         * for total duration accounting and (for the audio passthrough
         * below) as the source packet stream. Phase-1 compose
         * aggregates audio from just tracks[0]'s clips — the
         * multi-track audio mix lands with audio-mix-scheduler-wire. */
        const std::string& bottom_track_id = tl_.tracks[0].id;
        std::vector<std::size_t> bottom_clip_indices;
        bottom_clip_indices.reserve(tl_.clips.size());
        for (std::size_t i = 0; i < tl_.clips.size(); ++i) {
            if (tl_.clips[i].track_id == bottom_track_id) {
                bottom_clip_indices.push_back(i);
            }
        }
        if (bottom_clip_indices.empty()) {
            if (err) *err = "ComposeSink: bottom track has no clips";
            return ME_E_INVALID_ARG;
        }

        ReencodeOptions opts;
        opts.out_path           = common_.out_path;
        opts.container          = common_.container;
        opts.video_codec        = "h264";
        opts.audio_codec        = "aac";
        opts.video_bitrate_bps  = video_bitrate_;
        opts.audio_bitrate_bps  = audio_bitrate_;
        opts.cancel             = common_.cancel;
        opts.on_ratio           = common_.on_ratio;
        opts.pool               = pool_;
        opts.target_color_space = common_.target_color_space;

        /* bottom-track segments double as the audio source list — we
         * walk them in time order to feed the audio encoder after all
         * video frames are composed. Video decoders per track are
         * opened separately below. */
        for (std::size_t ci : bottom_clip_indices) {
            if (ci >= demuxes.size() || !demuxes[ci]) {
                if (err) *err = "ComposeSink: missing demux for bottom-track clip";
                return ME_E_INVALID_ARG;
            }
            opts.segments.push_back(ReencodeSegment{
                demuxes[ci],
                ranges_[ci].source_start,
                ranges_[ci].source_duration,
                ranges_[ci].source_color_space,
            });
        }

        /* --- Encoder + mux bootstrap ----------------------------- */
        std::unique_ptr<me::io::MuxContext> mux;
        me::resource::CodecPool::Ptr venc_owner, aenc_owner;
        detail::SharedEncState shared;
        if (auto s = setup_h264_aac_encoder_mux(
                opts, demuxes[0] ? demuxes[0]->fmt : nullptr,
                mux, venc_owner, aenc_owner, shared, err);
            s != ME_OK) {
            return s;
        }
        struct FifoGuard {
            AVAudioFifo* f;
            ~FifoGuard() { if (f) av_audio_fifo_free(f); }
        } fifo_guard{shared.afifo};

        if (auto s = mux->open_avio(err);    s != ME_OK) return s;
        if (auto s = mux->write_header(err); s != ME_OK) return s;

        /* AudioMixer setup (extracted to compose_audio.{hpp,cpp}) —
         * detects audio tracks + builds a mixer matching the AAC
         * encoder's params. Empty mixer (nullptr) = video-only
         * legacy flush. */
        std::unique_ptr<me::audio::AudioMixer> mixer;
        if (auto s = setup_compose_audio_mixer(
                tl_, demuxes, shared, pool_, mixer, err);
            s != ME_OK) {
            return s;
        }

        /* --- Per-clip video decoders ----------------------------
         * One TrackDecoderState per Timeline::clips entry, keyed by
         * clip_idx. Prior design opened one decoder per track (first
         * clip only), which silently broke multi-clip-per-track
         * compose and can't support cross-dissolve transitions that
         * need frames from BOTH endpoint clips simultaneously. Per-
         * clip indexing aligns with `TrackActive::clip_idx` from
         * active_clips_at / frame_source_at — the frame loop looks
         * up `clip_decoders[ta.clip_idx]` or `clip_decoders[
         * fs.transition_{from,to}_clip_idx]` directly. Decoders for
         * clips whose demux is absent or non-video stay default-
         * constructed and are skipped at pull time. */
        std::vector<TrackDecoderState> clip_decoders(tl_.clips.size());
        for (std::size_t ci = 0; ci < tl_.clips.size(); ++ci) {
            if (ci >= demuxes.size() || !demuxes[ci]) continue;
            if (auto s = open_track_decoder(demuxes[ci], *pool_,
                                             clip_decoders[ci], err);
                s != ME_OK) {
                return s;
            }
        }

        /* --- RGBA working buffers + encoder target frame --------- */
        const int W = shared.v_width;
        const int H = shared.v_height;
        if (W <= 0 || H <= 0) {
            if (err) *err = "ComposeSink: encoder has no video dimensions";
            return ME_E_INTERNAL;
        }
        std::vector<uint8_t> dst_rgba(static_cast<std::size_t>(W) * H * 4, 0);
        std::vector<uint8_t> track_rgba;
        /* Intermediate canvas-sized RGBA buffer for the affine-
         * transform path. Allocated once outside the per-frame loop;
         * affine_blit overwrites every pixel (ensuring no stale data
         * from prior frames). Only used when a clip has a non-
         * identity spatial Transform. */
        std::vector<uint8_t> track_rgba_xform(
            static_cast<std::size_t>(W) * H * 4, 0);

        /* Transition working buffers — used only when a track returns
         * FrameSourceKind::Transition at T (cross-dissolve window
         * covers T on that track). `from_rgba` and `to_rgba` each
         * hold the next decoded frame from the from_clip and to_clip
         * decoders respectively; `track_rgba` is reused for the
         * blended output (cross_dissolve writes here, then the
         * existing alpha_over path applies opacity onto dst_rgba). */
        std::vector<uint8_t> from_rgba;
        std::vector<uint8_t> to_rgba;
        /* Transition-path canvas-sized pre-transform scratches (one
         * per endpoint). Each clip's Transform is applied here before
         * cross_dissolve so from_clip.transform and to_clip.transform
         * both take effect during the blend — not just to_clip's. */
        std::vector<uint8_t> from_canvas;
        std::vector<uint8_t> to_canvas;

        me::io::AvFramePtr target_yuv(av_frame_alloc());
        if (!target_yuv) {
            if (err) *err = "ComposeSink: av_frame_alloc(target_yuv)";
            return ME_E_OUT_OF_MEMORY;
        }
        target_yuv->format = shared.venc_pix;
        target_yuv->width  = W;
        target_yuv->height = H;
        if (av_frame_get_buffer(target_yuv.get(), 32) < 0) {
            if (err) *err = "ComposeSink: av_frame_get_buffer(target_yuv)";
            return ME_E_OUT_OF_MEMORY;
        }

        /* --- Per-output-frame compose loop (extracted to
         * compose_decode_loop.cpp) --------------------------- */
        {
            ComposeVideoLoopCtx loop_ctx{
                tl_,
                clip_decoders,
                shared,
                W, H,
                dst_rgba,
                track_rgba,
                track_rgba_xform,
                from_rgba,
                to_rgba,
                from_canvas,
                to_canvas,
                target_yuv.get(),
            };
            if (auto s = run_compose_video_frame_loop(loop_ctx, err);
                s != ME_OK) {
                return s;
            }
        }

        /* --- Flush video encoder -------------------------------- */
        if (auto s = detail::encode_video_frame(
                /*in_frame=*/nullptr, shared.venc,
                nullptr, nullptr,
                shared.ofmt, shared.out_vidx,
                shared.venc->time_base, err);
            s != ME_OK) {
            return s;
        }

        /* Audio path (extracted to compose_audio.{hpp,cpp}) —
         * mixer-driven drain or legacy FIFO flush, plus common
         * null-frame flush. */
        if (auto s = drain_compose_audio(mixer.get(), shared, err);
            s != ME_OK) {
            return s;
        }

        /* --- Write trailer -------------------------------------- */
        if (auto s = mux->write_trailer(err); s != ME_OK) return s;

        if (shared.on_ratio) shared.on_ratio(1.0f);
        return ME_OK;
    }

private:
    const me::Timeline&        tl_;
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
    me::resource::CodecPool*   pool_ = nullptr;
    /* Borrowed; owned by me_engine. May be null (tests / engines that
     * predate the gpu_backend field). Not yet consulted by the CPU
     * compose path — `[[maybe_unused]]` silences -Wunused-private-field
     * until the effect-gpu-* + compose-sink-gpu-path cycles read it.
     * Plumbing the field now keeps that future cycle local to this
     * TU rather than cross-file surgery. */
    [[maybe_unused]] const me::gpu::GpuBackend* gpu_backend_ = nullptr;
    int64_t                    video_bitrate_ = 0;
    int64_t                    audio_bitrate_ = 0;
};

}  // namespace

namespace detail {

std::unique_ptr<OutputSink> make_compose_sink_impl(
    const me::Timeline&            tl,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    const me::gpu::GpuBackend*     gpu_backend,
    int64_t                        video_bitrate,
    int64_t                        audio_bitrate) {
    return std::make_unique<ComposeSink>(
        tl,
        std::move(common),
        std::move(clip_ranges),
        pool,
        gpu_backend,
        video_bitrate,
        audio_bitrate);
}

}  // namespace detail

}  // namespace me::orchestrator
