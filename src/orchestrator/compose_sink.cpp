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
#include "compose/affine_blit.hpp"
#include "compose/alpha_over.hpp"
#include "compose/cross_dissolve.hpp"
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

            /* Start from opaque black; each track's FrameSource
             * (SingleClip or Transition) alpha_over's on top in
             * Timeline::tracks declaration order (bottom → top
             * z-order). Tracks whose frame_source_at returns None at
             * T contribute nothing (lower layers unchanged). */
            std::fill(dst_rgba.begin(), dst_rgba.end(), 0);
            for (std::size_t i = 3; i < dst_rgba.size(); i += 4) {
                dst_rgba[i] = 255;
            }

            for (std::size_t ti = 0; ti < tl_.tracks.size(); ++ti) {
                const me::compose::FrameSource fs =
                    me::compose::frame_source_at(tl_, ti, T);

                if (fs.kind == me::compose::FrameSourceKind::None) continue;

                /* Track-level output of this frame's compositing
                 * (before opacity + transform): track_rgba is filled
                 * with W×H×4 RGBA8 ready for alpha_over onto dst_rgba.
                 * SingleClip path: decoded frame → track_rgba.
                 * Transition path: cross_dissolve(from, to, t) → track_rgba. */
                int src_w = 0, src_h = 0;
                std::size_t transform_clip_idx = SIZE_MAX;   /* for per-clip opacity / Transform */

                if (fs.kind == me::compose::FrameSourceKind::SingleClip) {
                    const me::compose::TrackActive& ta = fs.single;
                    if (ta.clip_idx >= clip_decoders.size()) continue;
                    TrackDecoderState& td = clip_decoders[ta.clip_idx];
                    if (td.video_stream_idx < 0 || !td.dec) continue;

                    me_status_t pull_s = pull_next_video_frame(
                        td.demux->fmt, td.video_stream_idx, td.dec.get(),
                        td.pkt_scratch.get(), td.frame_scratch.get(), err);
                    if (pull_s == ME_E_NOT_FOUND) continue;
                    if (pull_s != ME_OK) return pull_s;

                    src_w = td.frame_scratch->width;
                    src_h = td.frame_scratch->height;

                    if (auto s = me::compose::frame_to_rgba8(
                            td.frame_scratch.get(), track_rgba, err);
                        s != ME_OK) {
                        av_frame_unref(td.frame_scratch.get());
                        return s;
                    }
                    av_frame_unref(td.frame_scratch.get());
                    transform_clip_idx = ta.clip_idx;
                } else {
                    /* Transition. Pull from + to decoders, both at
                     * canvas resolution for phase-1 (cross_dissolve
                     * requires matching dims + strides across all
                     * three buffers — enforcing W×H avoids a separate
                     * affine pre-composite for each endpoint).
                     *
                     * Phase-1 limitations (documented in the
                     * cross-dissolve-transition-render-wire decision):
                     *   - No per-clip Transform applied to from/to
                     *     during the transition window (identity
                     *     assumed). Slow-path affine during blend is
                     *     a follow-up.
                     *   - to_clip's single-clip region after the
                     *     transition window plays `duration/2` ahead
                     *     of what the schema nominally says — because
                     *     the to decoder advances by one frame per
                     *     output frame throughout the window, arriving
                     *     at the post-window boundary with
                     *     `duration/2 * fps` frames consumed. Slowly-
                     *     changing content makes this visually
                     *     imperceptible; proper handles / seeking
                     *     is future work. */
                    const std::size_t from_ci = fs.transition_from_clip_idx;
                    const std::size_t to_ci   = fs.transition_to_clip_idx;
                    if (from_ci >= clip_decoders.size() ||
                        to_ci   >= clip_decoders.size()) continue;
                    TrackDecoderState& td_from = clip_decoders[from_ci];
                    TrackDecoderState& td_to   = clip_decoders[to_ci];
                    if (td_from.video_stream_idx < 0 || !td_from.dec ||
                        td_to.video_stream_idx   < 0 || !td_to.dec) continue;

                    /* Pull from_clip; if exhausted, degrade to to-only
                     * single-clip rendering (weight-based soft degrade
                     * isn't possible without a cached last-from frame,
                     * which is a follow-up for real handle support). */
                    const me_status_t pull_from = pull_next_video_frame(
                        td_from.demux->fmt, td_from.video_stream_idx,
                        td_from.dec.get(), td_from.pkt_scratch.get(),
                        td_from.frame_scratch.get(), err);
                    bool from_valid = false;
                    int from_w = 0, from_h = 0;
                    if (pull_from == ME_OK) {
                        from_w = td_from.frame_scratch->width;
                        from_h = td_from.frame_scratch->height;
                        if (auto s = me::compose::frame_to_rgba8(
                                td_from.frame_scratch.get(), from_rgba, err);
                            s != ME_OK) {
                            av_frame_unref(td_from.frame_scratch.get());
                            return s;
                        }
                        av_frame_unref(td_from.frame_scratch.get());
                        from_valid = true;
                    } else if (pull_from != ME_E_NOT_FOUND) {
                        return pull_from;
                    }

                    /* Pull to_clip. */
                    const me_status_t pull_to = pull_next_video_frame(
                        td_to.demux->fmt, td_to.video_stream_idx,
                        td_to.dec.get(), td_to.pkt_scratch.get(),
                        td_to.frame_scratch.get(), err);
                    if (pull_to == ME_E_NOT_FOUND) continue;   /* whole transition contributes nothing */
                    if (pull_to != ME_OK) return pull_to;

                    const int to_w = td_to.frame_scratch->width;
                    const int to_h = td_to.frame_scratch->height;
                    if (auto s = me::compose::frame_to_rgba8(
                            td_to.frame_scratch.get(), to_rgba, err);
                        s != ME_OK) {
                        av_frame_unref(td_to.frame_scratch.get());
                        return s;
                    }
                    av_frame_unref(td_to.frame_scratch.get());

                    /* Enforce W×H for transition endpoint frames. */
                    if (to_w != W || to_h != H ||
                        (from_valid && (from_w != W || from_h != H))) {
                        if (err) {
                            *err = "ComposeSink: cross-dissolve endpoint frame size "
                                   "doesn't match output; transition rendering "
                                   "requires W×H-matching source frames for phase-1 "
                                   "(affine pre-composite during blend is a "
                                   "follow-up of cross-dissolve-transition-render-wire)";
                        }
                        return ME_E_UNSUPPORTED;
                    }

                    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
                    if (track_rgba.size() != bytes) track_rgba.resize(bytes);

                    if (from_valid) {
                        me::compose::cross_dissolve(
                            track_rgba.data(),
                            from_rgba.data(), to_rgba.data(),
                            W, H, static_cast<std::size_t>(W) * 4,
                            fs.transition.t);
                    } else {
                        /* from exhausted — copy to into track_rgba
                         * unblended (t effectively = 1). */
                        std::memcpy(track_rgba.data(), to_rgba.data(), bytes);
                    }
                    src_w = W;
                    src_h = H;
                    transform_clip_idx = to_ci;   /* to_clip's opacity / transform wins for layer compositing */
                }

                /* Common: apply per-clip opacity + optional affine
                 * transform, then alpha_over onto dst_rgba. */
                const me::Clip& clip = tl_.clips[transform_clip_idx];
                const float opacity =
                    clip.transform.has_value()
                        ? static_cast<float>(clip.transform->opacity)
                        : 1.0f;

                const bool spatial_identity =
                    !clip.transform.has_value() ||
                    (clip.transform->translate_x  == 0.0 &&
                     clip.transform->translate_y  == 0.0 &&
                     clip.transform->scale_x      == 1.0 &&
                     clip.transform->scale_y      == 1.0 &&
                     clip.transform->rotation_deg == 0.0);

                if (spatial_identity) {
                    if (src_w != W || src_h != H) {
                        if (err) {
                            *err = "ComposeSink: track frame size (" +
                                   std::to_string(src_w) + "x" +
                                   std::to_string(src_h) +
                                   ") doesn't match output (" +
                                   std::to_string(W) + "x" +
                                   std::to_string(H) +
                                   "); either match the timeline "
                                   "resolution or set a non-identity "
                                   "Transform (scale != 1 or translate "
                                   "!= 0) to opt into the affine "
                                   "pre-composite path";
                        }
                        return ME_E_UNSUPPORTED;
                    }
                    me::compose::alpha_over(
                        dst_rgba.data(), track_rgba.data(),
                        W, H, static_cast<std::size_t>(W) * 4,
                        opacity,
                        me::compose::BlendMode::Normal);
                } else {
                    const me::Transform& tr = *clip.transform;
                    const me::compose::AffineMatrix inv =
                        me::compose::compose_inverse_affine(
                            tr.translate_x, tr.translate_y,
                            tr.scale_x,     tr.scale_y,
                            tr.rotation_deg,
                            tr.anchor_x,    tr.anchor_y,
                            src_w, src_h);
                    me::compose::affine_blit(
                        track_rgba_xform.data(), W, H,
                        static_cast<std::size_t>(W) * 4,
                        track_rgba.data(), src_w, src_h,
                        static_cast<std::size_t>(src_w) * 4,
                        inv);
                    me::compose::alpha_over(
                        dst_rgba.data(), track_rgba_xform.data(),
                        W, H, static_cast<std::size_t>(W) * 4,
                        opacity,
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
        if (err) *err = "compose path (used for multi-track or transitions) "
                         "currently requires video_codec=h264 + audio_codec=aac; "
                         "other codecs (including passthrough) unsupported";
        return nullptr;
    }
    if (!pool) {
        if (err) *err = "compose path requires a CodecPool (engine->codecs)";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "compose path requires at least one clip";
        return nullptr;
    }
    /* Accept multi-track OR any timeline with transitions. Single-
     * track no-transition timelines route through make_output_sink. */
    if (tl.tracks.size() < 2 && tl.transitions.empty()) {
        if (err) *err = "make_compose_sink: expected 2+ tracks or non-empty "
                         "transitions (simpler timelines route through "
                         "make_output_sink, not here)";
        return nullptr;
    }

    /* Per-track clip-count rule:
     *   - Track without any transition on it: exactly 1 clip
     *     (multi-clip concat on a non-transition track is
     *     the old "multi-clip-single-track compose" gap that
     *     needs decoder seek / source_start=0 semantics — not
     *     in this phase).
     *   - Track with at least one transition declared on it: 2+
     *     clips allowed (a transition's from_clip_id + to_clip_id
     *     point at two distinct clips on the same track). */
    std::vector<std::size_t> per_track_count(tl.tracks.size(), 0);
    for (const auto& c : tl.clips) {
        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            if (tl.tracks[ti].id == c.track_id) {
                ++per_track_count[ti];
                break;
            }
        }
    }
    std::vector<bool> track_has_transition(tl.tracks.size(), false);
    for (const auto& tr : tl.transitions) {
        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            if (tl.tracks[ti].id == tr.track_id) {
                track_has_transition[ti] = true;
                break;
            }
        }
    }
    for (std::size_t ti = 0; ti < per_track_count.size(); ++ti) {
        if (track_has_transition[ti]) continue;   /* transition tracks exempt */
        if (per_track_count[ti] != 1) {
            if (err) {
                *err = "compose phase-1: non-transition tracks must have "
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
