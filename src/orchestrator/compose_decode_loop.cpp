#include "orchestrator/compose_decode_loop.hpp"

#include "compose/active_clips.hpp"
#include "compose/affine_blit.hpp"
#include "compose/alpha_over.hpp"
#include "compose/frame_convert.hpp"
#include "orchestrator/compose_transition_step.hpp"
#include "orchestrator/reencode_video.hpp"

#ifdef ME_HAS_SKIA
#include "text/text_renderer.hpp"
#endif

#ifdef ME_HAS_LIBASS
#include "text/subtitle_renderer.hpp"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace me::orchestrator {

me_status_t run_compose_video_frame_loop(
    ComposeVideoLoopCtx& ctx,
    std::string*         err) {

    const me::Timeline& tl      = ctx.tl;
    auto& clip_decoders         = ctx.clip_decoders;
    auto& shared                = ctx.shared;
    const int W                 = ctx.W;
    const int H                 = ctx.H;

    /* total_frames = ceil(duration * fps). duration = tl.duration,
     * fps = tl.frame_rate. Cross-multiply in int64 keeps the
     * ceiling precise for rational inputs. */
    const int64_t total_frames =
        (tl.duration.num * tl.frame_rate.num + tl.duration.den * tl.frame_rate.den - 1) /
        (tl.duration.den * tl.frame_rate.den);

#ifdef ME_HAS_SKIA
    /* Per-text-clip lazy-init renderer cache. TextClipParams live on
     * `Clip::text_params`; we key by clip_idx so each text clip gets
     * its own TextRenderer (canvas is W×H for all; Skia surface alloc
     * isn't free — avoid rebuilding per frame). Non-text clips leave
     * their slot null — zero-cost for video-only timelines. */
    std::vector<std::unique_ptr<me::text::TextRenderer>> text_renderers(tl.clips.size());
#endif

#ifdef ME_HAS_LIBASS
    /* Per-subtitle-clip lazy-init renderer cache. libass state
     * (ASS_Library + ASS_Renderer + ASS_Track) is more expensive to
     * build than Skia's text surface — loading a .ass/.srt track
     * parses the whole file. Cache per clip_idx + load_from_memory
     * exactly once on the first frame we enter the clip's range. */
    std::vector<std::unique_ptr<me::text::SubtitleRenderer>> subtitle_renderers(tl.clips.size());
#endif

    for (int64_t fi = 0; fi < total_frames; ++fi) {
        if (shared.cancel &&
            shared.cancel->load(std::memory_order_acquire)) {
            return ME_E_CANCELLED;
        }
        /* T = fi / fps in rational form (fi * fr.den / fr.num).
         * Keep both num/den integral; active_clips_at does rational
         * compare. */
        const me_rational_t T{
            fi * tl.frame_rate.den,
            tl.frame_rate.num,
        };

        /* Start from opaque black; each track's FrameSource
         * (SingleClip or Transition) alpha_over's on top in
         * Timeline::tracks declaration order (bottom → top
         * z-order). Tracks whose frame_source_at returns None at
         * T contribute nothing (lower layers unchanged). */
        std::fill(ctx.dst_rgba.begin(), ctx.dst_rgba.end(), 0);
        for (std::size_t i = 3; i < ctx.dst_rgba.size(); i += 4) {
            ctx.dst_rgba[i] = 255;
        }

        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            /* Skip audio tracks in the video compose loop —
             * their per-clip video decoder (if even opened for
             * a file that happens to have video) would show
             * stray frames during audio-track time ranges. The
             * AudioMixer handles the audio-track content
             * separately. */
            if (tl.tracks[ti].kind == me::TrackKind::Audio) continue;

            const me::compose::FrameSource fs =
                me::compose::frame_source_at(tl, ti, T);

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

                const me::Clip& cur_clip = tl.clips[ta.clip_idx];
                bool text_handled = false;

#ifdef ME_HAS_SKIA
                /* Text clip: synthetic, no decoder. Lazy-init the
                 * TextRenderer at the output canvas size and draw the
                 * current clip params at T onto track_rgba. Size it
                 * here (not outside the branch) to keep the vector
                 * usable below without a parallel size check. */
                if (cur_clip.type == me::ClipType::Text &&
                    cur_clip.text_params.has_value()) {
                    auto& tr = text_renderers[ta.clip_idx];
                    if (!tr) {
                        tr = std::make_unique<me::text::TextRenderer>(W, H);
                    }
                    const std::size_t pitch =
                        static_cast<std::size_t>(W) * 4;
                    ctx.track_rgba.assign(pitch * static_cast<std::size_t>(H), 0);
                    if (tr->valid()) {
                        tr->render(*cur_clip.text_params, T,
                                    ctx.track_rgba.data(), pitch);
                    }
                    src_w = W;
                    src_h = H;
                    transform_clip_idx = ta.clip_idx;
                    text_handled = true;
                }
#endif

#ifdef ME_HAS_LIBASS
                /* Subtitle clip: synthetic, no decoder. Lazy-init
                 * the SubtitleRenderer + parse inline .ass/.srt on
                 * first visit; subsequent frames reuse the parsed
                 * track and only the t_ms → render_frame call runs.
                 * libass consumes time in milliseconds — convert T
                 * to integer ms via rational-safe scaling. */
                if (!text_handled &&
                    cur_clip.type == me::ClipType::Subtitle &&
                    cur_clip.subtitle_params.has_value()) {
                    auto& sr = subtitle_renderers[ta.clip_idx];
                    if (!sr) {
                        sr = std::make_unique<me::text::SubtitleRenderer>(W, H);
                        const auto& sp = *cur_clip.subtitle_params;
                        /* Source the subtitle bytes either from the
                         * inline `content` string or by reading the
                         * file referenced by `file_uri`. Loader
                         * ensures exactly one is populated. Inline
                         * empty content is a valid no-op (empty
                         * subtitle track); file_uri that fails to
                         * open is surfaced to err so hosts can
                         * diagnose the bad path via
                         * me_engine_last_error. */
                        std::string bytes;
                        if (!sp.content.empty()) {
                            bytes = sp.content;
                        } else if (!sp.file_uri.empty()) {
                            std::string path = sp.file_uri;
                            constexpr std::string_view file_prefix{"file://"};
                            if (path.size() > file_prefix.size() &&
                                path.compare(0, file_prefix.size(), file_prefix) == 0) {
                                path = path.substr(file_prefix.size());
                            }
                            std::ifstream in(path, std::ios::binary);
                            if (!in) {
                                if (err) {
                                    *err = "subtitle file_uri not readable: '" +
                                           sp.file_uri + "'";
                                }
                                return ME_E_IO;
                            }
                            std::ostringstream ss;
                            ss << in.rdbuf();
                            bytes = ss.str();
                        }
                        if (!bytes.empty()) {
                            sr->load_from_memory(bytes,
                                sp.codepage.empty() ? nullptr : sp.codepage.c_str());
                        }
                    }
                    const std::size_t pitch =
                        static_cast<std::size_t>(W) * 4;
                    ctx.track_rgba.assign(pitch * static_cast<std::size_t>(H), 0);
                    if (sr->valid()) {
                        /* t_ms = T.num * 1000 / T.den; rational input
                         * is exact, cast to int64 is the natural
                         * libass boundary. */
                        const int64_t t_ms = (T.num * 1000) / T.den;
                        sr->render_frame(t_ms, ctx.track_rgba.data(), pitch);
                    }
                    src_w = W;
                    src_h = H;
                    transform_clip_idx = ta.clip_idx;
                    text_handled = true;
                }
#endif

                if (!text_handled) {
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
                        td.frame_scratch.get(), ctx.track_rgba, err);
                    s != ME_OK) {
                    av_frame_unref(td.frame_scratch.get());
                    return s;
                }
                av_frame_unref(td.frame_scratch.get());
                transform_clip_idx = ta.clip_idx;
                }  /* !text_handled */
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

                const me::Clip& from_clip = tl.clips[from_ci];
                const me::Clip& to_clip   = tl.clips[to_ci];
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
                    ctx.track_rgba, ctx.from_rgba, ctx.to_rgba,
                    ctx.from_canvas, ctx.to_canvas,
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
            const me::Clip& clip = tl.clips[transform_clip_idx];
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
                    ctx.dst_rgba.data(), ctx.track_rgba.data(),
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
                    ctx.track_rgba_xform.data(), W, H,
                    static_cast<std::size_t>(W) * 4,
                    ctx.track_rgba.data(), src_w, src_h,
                    static_cast<std::size_t>(src_w) * 4,
                    inv);
                me::compose::alpha_over(
                    ctx.dst_rgba.data(), ctx.track_rgba_xform.data(),
                    W, H, static_cast<std::size_t>(W) * 4,
                    opacity,
                    me::compose::BlendMode::Normal);
            }
        }

        if (auto s = me::compose::rgba8_to_frame(
                ctx.dst_rgba.data(), W, H,
                static_cast<std::size_t>(W) * 4,
                ctx.target_yuv, err);
            s != ME_OK) {
            return s;
        }

        ctx.target_yuv->pts     = shared.next_video_pts;
        ctx.target_yuv->pkt_dts = shared.next_video_pts;
        shared.next_video_pts  += shared.video_pts_delta;

        if (auto s = detail::encode_video_frame(
                ctx.target_yuv, shared.venc,
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

    return ME_OK;
}

}  // namespace me::orchestrator
