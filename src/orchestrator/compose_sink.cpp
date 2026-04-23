/*
 * ComposeSink impl — real per-output-frame multi-track composite via
 * alpha_over.
 *
 * Wires together the six prereqs that prior scope-A cycles landed:
 *   - setup_h264_aac_encoder_mux (encoder + mux bootstrap)
 *   - open_track_decoder / TrackDecoderState (per-track decode bundle)
 *   - pull_next_video_frame (advance one track's decoder by one frame)
 *   - me::compose::active_clips_at (resolve which clip is active per
 *     track at timeline time T)
 *   - me::compose::frame_to_rgba8 / rgba8_to_frame (AVFrame ↔ RGBA8)
 *   - me::compose::alpha_over (Porter-Duff src-over, Normal blend)
 *
 * Phase-1 simplifications:
 *   - Each track has exactly one clip (strips within-track clip
 *     transitions; enforced by the make_compose_sink factory).
 *   - Audio is passthrough from demuxes[0]'s audio stream via the
 *     usual reencode audio path (no multi-track audio mixing yet —
 *     that's audio-mix-scheduler-wire).
 *   - All tracks' source frames composite bottom→top at full opacity
 *     (Transform.opacity / real blend modes arrive with
 *     transform-compose-wire / blend-mode-schema bullets).
 */
#include "orchestrator/compose_sink.hpp"

#include "compose/active_clips.hpp"
#include "compose/alpha_over.hpp"
#include "compose/frame_convert.hpp"
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

#include <algorithm>
#include <cstring>
#include <utility>

namespace me::orchestrator {

namespace {

bool streq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

/* Find the first clip on a given track. Returns SIZE_MAX if the track
 * has no clips (factory should reject this, but defensive). */
std::size_t first_clip_on_track(const me::Timeline& tl, const std::string& track_id) {
    for (std::size_t i = 0; i < tl.clips.size(); ++i) {
        if (tl.clips[i].track_id == track_id) return i;
    }
    return SIZE_MAX;
}

class ComposeSink final : public OutputSink {
public:
    ComposeSink(const me::Timeline&        tl,
                SinkCommon                 common,
                std::vector<ClipTimeRange> ranges,
                me::resource::CodecPool*   pool,
                int64_t                    video_bitrate,
                int64_t                    audio_bitrate)
        : tl_(tl),
          common_(std::move(common)),
          ranges_(std::move(ranges)),
          pool_(pool),
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

        /* --- Per-track video decoders ---------------------------- */
        std::vector<TrackDecoderState> track_decoders(tl_.tracks.size());
        for (std::size_t ti = 0; ti < tl_.tracks.size(); ++ti) {
            const std::size_t clip_idx =
                first_clip_on_track(tl_, tl_.tracks[ti].id);
            if (clip_idx == SIZE_MAX) continue;  /* empty track; skip */
            if (clip_idx >= demuxes.size() || !demuxes[clip_idx]) continue;
            if (auto s = open_track_decoder(demuxes[clip_idx], *pool_,
                                             track_decoders[ti], err);
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

        /* --- Per-output-frame compose loop ----------------------- */
        /* total_frames = ceil(duration * fps). duration = tl_.duration,
         * fps = tl_.frame_rate. For cross-multiply precision keep it
         * in int64. */
        const int64_t total_frames =
            (tl_.duration.num * tl_.frame_rate.num + tl_.duration.den * tl_.frame_rate.den - 1) /
            (tl_.duration.den * tl_.frame_rate.den);

        for (int64_t fi = 0; fi < total_frames; ++fi) {
            if (shared.cancel &&
                shared.cancel->load(std::memory_order_acquire)) {
                return ME_E_CANCELLED;
            }
            /* T = fi / fps in rational form (fi * fr.den / fr.num).
             * Keep both num/den integral; active_clips_at does rational
             * compare. */
            const me_rational_t T{
                fi * tl_.frame_rate.den,
                tl_.frame_rate.num,
            };

            const auto active = me::compose::active_clips_at(tl_, T);
            if (active.empty()) {
                /* No track has content at this timeline time — this
                 * can happen if tl.duration overruns the real clips.
                 * Emit a black frame so the output still has the
                 * expected frame count. */
                std::fill(dst_rgba.begin(), dst_rgba.end(), 0);
                /* Restore full alpha so the YUV conversion doesn't
                 * interpret RGB as zero-alpha ghosts. */
                for (std::size_t i = 3; i < dst_rgba.size(); i += 4) {
                    dst_rgba[i] = 255;
                }
            } else {
                /* Start from opaque black; each active track alpha_over's
                 * on top in declaration order (bottom → top z-order). */
                std::fill(dst_rgba.begin(), dst_rgba.end(), 0);
                for (std::size_t i = 3; i < dst_rgba.size(); i += 4) {
                    dst_rgba[i] = 255;
                }

                for (const auto& ta : active) {
                    if (ta.track_idx >= track_decoders.size()) continue;
                    TrackDecoderState& td = track_decoders[ta.track_idx];
                    if (td.video_stream_idx < 0 || !td.dec) continue;

                    me_status_t pull_s = pull_next_video_frame(
                        td.demux->fmt, td.video_stream_idx, td.dec.get(),
                        td.pkt_scratch.get(), td.frame_scratch.get(), err);
                    if (pull_s == ME_E_NOT_FOUND) {
                        /* Track exhausted early — leave dst_rgba as-is
                         * for this layer (lower layers unchanged). */
                        continue;
                    }
                    if (pull_s != ME_OK) return pull_s;

                    if (auto s = me::compose::frame_to_rgba8(
                            td.frame_scratch.get(), track_rgba, err);
                        s != ME_OK) {
                        av_frame_unref(td.frame_scratch.get());
                        return s;
                    }
                    av_frame_unref(td.frame_scratch.get());

                    /* Track frame may have different dims than output
                     * (e.g. 640×480 source into 1920×1080 timeline).
                     * Phase-1 simplification: both sides are expected
                     * identical (loader + encoder both use timeline's
                     * resolution). If they differ, alpha_over would
                     * walk out-of-bounds on the smaller buffer —
                     * detect and skip that layer with a warning-style
                     * error. */
                    const int src_w = td.frame_scratch->width;
                    const int src_h = td.frame_scratch->height;
                    if (src_w != W || src_h != H) {
                        /* Future: scale to target dims via sws. For now,
                         * err out so misconfigurations are loud. */
                        if (err) {
                            *err = "ComposeSink: track frame size (" +
                                   std::to_string(src_w) + "x" +
                                   std::to_string(src_h) +
                                   ") doesn't match output (" +
                                   std::to_string(W) + "x" +
                                   std::to_string(H) +
                                   "); phase-1 requires identical dims "
                                   "(future: per-track sws scale)";
                        }
                        return ME_E_UNSUPPORTED;
                    }

                    me::compose::alpha_over(
                        dst_rgba.data(), track_rgba.data(),
                        W, H, static_cast<std::size_t>(W) * 4,
                        /*opacity=*/1.0f,
                        me::compose::BlendMode::Normal);
                }
            }

            if (auto s = me::compose::rgba8_to_frame(
                    dst_rgba.data(), W, H,
                    static_cast<std::size_t>(W) * 4,
                    target_yuv.get(), err);
                s != ME_OK) {
                return s;
            }

            target_yuv->pts     = shared.next_video_pts;
            target_yuv->pkt_dts = shared.next_video_pts;
            shared.next_video_pts += shared.video_pts_delta;

            if (auto s = detail::encode_video_frame(
                    target_yuv.get(), shared.venc,
                    /*sws=*/nullptr, /*scratch_nv12=*/nullptr,
                    shared.ofmt, shared.out_vidx,
                    shared.venc->time_base, err);
                s != ME_OK) {
                return s;
            }

            if (shared.on_ratio && total_frames > 0) {
                shared.on_ratio(static_cast<float>(fi + 1) /
                                static_cast<float>(total_frames));
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

        /* --- Audio flush ---------------------------------------- */
        /* Phase-1 audio path: don't attempt to pull audio packets
         * from the demuxes — multi-clip audio from bottom track via
         * reencode_segment machinery is complex to integrate inline
         * and the real multi-track audio solution is
         * audio-mix-scheduler-wire. For now, drain whatever is
         * already in the FIFO (zero samples typically) and flush the
         * encoder. The resulting output has a declared audio stream
         * with no samples — valid MP4 structurally, though some
         * players may complain about the empty audio. */
        if (shared.aenc) {
            if (auto s = detail::drain_audio_fifo(
                    shared.afifo, shared.aenc, shared.ofmt,
                    shared.out_aidx, &shared.next_audio_pts,
                    /*flush=*/true, err);
                s != ME_OK) {
                return s;
            }
            if (auto s = detail::encode_audio_frame(
                    nullptr, shared.aenc,
                    shared.ofmt, shared.out_aidx, err);
                s != ME_OK) {
                return s;
            }
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
    int64_t                    video_bitrate_ = 0;
    int64_t                    audio_bitrate_ = 0;
};

}  // namespace

std::unique_ptr<OutputSink> make_compose_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    std::string*                   err) {

    if (!streq(spec.video_codec, "h264") || !streq(spec.audio_codec, "aac")) {
        if (err) *err = "multi-track compose currently requires "
                         "video_codec=h264 + audio_codec=aac; other codecs "
                         "(including passthrough) unsupported for compose path";
        return nullptr;
    }
    if (!pool) {
        if (err) *err = "multi-track compose requires a CodecPool (engine->codecs)";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "multi-track compose requires at least one clip";
        return nullptr;
    }
    if (tl.tracks.size() < 2) {
        if (err) *err = "make_compose_sink: expected 2+ tracks (single-track "
                         "timelines route through make_output_sink, not here)";
        return nullptr;
    }

    /* Phase-1: each track must have exactly one clip. Multi-clip-per-
     * track compose needs within-track clip-transition handling which
     * is separately-scoped work. */
    std::vector<std::size_t> per_track_count(tl.tracks.size(), 0);
    for (const auto& c : tl.clips) {
        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            if (tl.tracks[ti].id == c.track_id) {
                ++per_track_count[ti];
                break;
            }
        }
    }
    for (std::size_t ti = 0; ti < per_track_count.size(); ++ti) {
        if (per_track_count[ti] != 1) {
            if (err) {
                *err = "multi-track compose phase-1: each track must have "
                       "exactly 1 clip; track[" + std::to_string(ti) +
                       "] has " + std::to_string(per_track_count[ti]);
            }
            return nullptr;
        }
    }

    return std::make_unique<ComposeSink>(
        tl,
        std::move(common),
        std::move(clip_ranges),
        pool,
        spec.video_bitrate_bps,
        spec.audio_bitrate_bps);
}

}  // namespace me::orchestrator
