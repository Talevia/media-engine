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

#include "audio/mixer.hpp"
#include "orchestrator/compose_audio.hpp"
#include "compose/active_clips.hpp"
#include "compose/affine_blit.hpp"
#include "compose/alpha_over.hpp"
#include "compose/cross_dissolve.hpp"
#include "compose/frame_convert.hpp"
#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/compose_transition_step.hpp"
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
                /* Skip audio tracks in the video compose loop —
                 * their per-clip video decoder (if even opened for
                 * a file that happens to have video) would show
                 * stray frames during audio-track time ranges. The
                 * AudioMixer handles the audio-track content
                 * separately. */
                if (tl_.tracks[ti].kind == me::TrackKind::Audio) continue;

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
                bool spatial_already_applied = false;        /* true when transition_step pre-transformed endpoints */

                if (fs.kind == me::compose::FrameSourceKind::SingleClip) {
                    const me::compose::TrackActive& ta = fs.single;
                    if (ta.clip_idx >= clip_decoders.size()) continue;
                    TrackDecoderState& td = clip_decoders[ta.clip_idx];
                    if (td.video_stream_idx < 0 || !td.dec) continue;

                    /* If this clip just came out of a cross-dissolve
                     * window as the to_clip endpoint, the decoder is
                     * `duration/2 × fps` frames ahead of schema (the
                     * transition advanced it once per emitted output
                     * frame across the whole window). Frame-accurate
                     * seek to ta.source_time — which is schema-aligned
                     * for this T — and the returned frame supplies
                     * this iteration's content; subsequent SingleClip
                     * pulls continue sequentially. */
                    me_status_t pull_s = ME_OK;
                    if (td.used_as_to_in_transition) {
                        td.used_as_to_in_transition = false;
                        pull_s = seek_track_decoder_frame_accurate_to(
                            td, ta.source_time, err);
                    } else {
                        pull_s = pull_next_video_frame(
                            td.demux->fmt, td.video_stream_idx, td.dec.get(),
                            td.pkt_scratch.get(), td.frame_scratch.get(), err);
                    }
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
                    /* Transition. Delegate to compose_transition_step
                     * (free function in compose_transition_step.cpp —
                     * scope-A slice of debt-split-compose-sink-cpp). */
                    const std::size_t from_ci = fs.transition_from_clip_idx;
                    const std::size_t to_ci   = fs.transition_to_clip_idx;
                    if (from_ci >= clip_decoders.size() ||
                        to_ci   >= clip_decoders.size()) continue;
                    TrackDecoderState& td_from = clip_decoders[from_ci];
                    TrackDecoderState& td_to   = clip_decoders[to_ci];
                    if (td_from.video_stream_idx < 0 || !td_from.dec ||
                        td_to.video_stream_idx   < 0 || !td_to.dec) continue;

                    const me::Clip& from_clip = tl_.clips[from_ci];
                    const me::Clip& to_clip   = tl_.clips[to_ci];
                    const me::TransformEvaluated from_tr =
                        from_clip.transform.has_value()
                            ? from_clip.transform->evaluate_at(T)
                            : me::TransformEvaluated{};
                    const me::TransformEvaluated to_tr =
                        to_clip.transform.has_value()
                            ? to_clip.transform->evaluate_at(T)
                            : me::TransformEvaluated{};
                    const me_status_t trs = compose_transition_step(
                        fs, from_tr, from_clip.transform.has_value(),
                            to_tr,   to_clip.transform.has_value(),
                        td_from, td_to, W, H,
                        track_rgba, from_rgba, to_rgba,
                        from_canvas, to_canvas,
                        src_w, src_h, transform_clip_idx,
                        spatial_already_applied, err);
                    if (trs == ME_E_NOT_FOUND) continue;
                    if (trs != ME_OK) return trs;
                }

                /* Common: apply per-clip opacity + optional affine
                 * transform, then alpha_over onto dst_rgba. When
                 * transition_step already pre-transformed both
                 * endpoints (`spatial_already_applied`), we skip our
                 * own spatial affine — track_rgba is at W×H with
                 * both clips' transforms baked in. Opacity still
                 * applies via alpha_over (cross_dissolve doesn't
                 * touch alpha). */
                const me::Clip& clip = tl_.clips[transform_clip_idx];
                const me::TransformEvaluated tr_eval =
                    clip.transform.has_value()
                        ? clip.transform->evaluate_at(T)
                        : me::TransformEvaluated{};
                const float opacity = static_cast<float>(tr_eval.opacity);

                const bool spatial_identity =
                    spatial_already_applied ||
                    !clip.transform.has_value() ||
                    tr_eval.spatial_identity();

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
                    const me::compose::AffineMatrix inv =
                        me::compose::compose_inverse_affine(
                            tr_eval.translate_x, tr_eval.translate_y,
                            tr_eval.scale_x,     tr_eval.scale_y,
                            tr_eval.rotation_deg,
                            tr_eval.anchor_x,    tr_eval.anchor_y,
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
