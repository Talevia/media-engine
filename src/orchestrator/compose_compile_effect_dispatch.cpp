/* compose_compile_effect_dispatch impl. See header for the
 * contract. */
#include "orchestrator/compose_compile_effect_dispatch.hpp"

#include "task/task_kind.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator::detail {

graph::PortRef append_clip_effects(graph::Graph::Builder& b,
                                    const me::Timeline&    tl,
                                    const me::Clip&        clip,
                                    graph::PortRef         prev,
                                    me_rational_t          time) {
    for (const auto& fx : clip.effects) {
        if (!fx.enabled) continue;
        if (fx.kind == me::EffectKind::FaceSticker) {
            const auto* params = std::get_if<me::FaceStickerEffectParams>(&fx.params);
            if (!params) continue;
            /* Resolve landmark asset id → URI via the timeline's
             * assets map. The compose stage's resolver expects a
             * file URI; the timeline JSON gives us asset ids that
             * indirect through the assets map. */
            std::string landmark_uri;
            auto it = tl.assets.find(params->landmark.asset_id);
            if (it != tl.assets.end()) landmark_uri = it->second.uri;

            graph::Properties fp;
            fp["sticker_uri"].v        = params->sticker_uri;
            fp["landmark_asset_uri"].v = landmark_uri;
            fp["frame_t_num"].v        = static_cast<int64_t>(time.num);
            fp["frame_t_den"].v        = static_cast<int64_t>(time.den);
            fp["scale_x"].v            = params->scale_x;
            fp["scale_y"].v            = params->scale_y;
            fp["offset_x"].v           = params->offset_x;
            fp["offset_y"].v           = params->offset_y;
            auto n = b.add(task::TaskKindId::RenderFaceSticker,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::BodyAlphaKey) {
            const auto* params = std::get_if<me::BodyAlphaKeyEffectParams>(&fx.params);
            if (!params) continue;
            std::string mask_uri;
            auto it = tl.assets.find(params->mask.asset_id);
            if (it != tl.assets.end()) mask_uri = it->second.uri;

            graph::Properties fp;
            fp["mask_asset_uri"].v    = mask_uri;
            fp["frame_t_num"].v       = static_cast<int64_t>(time.num);
            fp["frame_t_den"].v       = static_cast<int64_t>(time.den);
            fp["feather_radius_px"].v = static_cast<int64_t>(params->feather_radius_px);
            fp["invert"].v            = static_cast<int64_t>(params->invert ? 1 : 0);
            auto n = b.add(task::TaskKindId::RenderBodyAlphaKey,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::Posterize) {
            const auto* params = std::get_if<me::PosterizeEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["levels"].v = static_cast<int64_t>(params->levels);
            auto n = b.add(task::TaskKindId::RenderPosterize,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::OrderedDither) {
            const auto* params = std::get_if<me::OrderedDitherEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["matrix_size"].v = static_cast<int64_t>(params->matrix_size);
            fp["levels"].v      = static_cast<int64_t>(params->levels);
            auto n = b.add(task::TaskKindId::RenderOrderedDither,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::MotionBlur) {
            const auto* params = std::get_if<me::MotionBlurEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["dx_px"].v   = static_cast<int64_t>(params->dx_px);
            fp["dy_px"].v   = static_cast<int64_t>(params->dy_px);
            fp["samples"].v = static_cast<int64_t>(params->samples);
            auto n = b.add(task::TaskKindId::RenderMotionBlur,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::RadialBlur) {
            const auto* params = std::get_if<me::RadialBlurEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["center_x"].v  = static_cast<double>(params->center_x);
            fp["center_y"].v  = static_cast<double>(params->center_y);
            fp["intensity"].v = static_cast<double>(params->intensity);
            fp["samples"].v   = static_cast<int64_t>(params->samples);
            auto n = b.add(task::TaskKindId::RenderRadialBlur,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::TiltShift) {
            const auto* params = std::get_if<me::TiltShiftEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["focal_y_min"].v     = static_cast<double>(params->focal_y_min);
            fp["focal_y_max"].v     = static_cast<double>(params->focal_y_max);
            fp["edge_softness"].v   = static_cast<double>(params->edge_softness);
            fp["max_blur_radius"].v = static_cast<int64_t>(params->max_blur_radius);
            auto n = b.add(task::TaskKindId::RenderTiltShift,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::Displacement) {
            const auto* params = std::get_if<me::DisplacementEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["texture_uri"].v = params->texture_uri;
            fp["strength_x"].v  = static_cast<double>(params->strength_x);
            fp["strength_y"].v  = static_cast<double>(params->strength_y);
            auto n = b.add(task::TaskKindId::RenderDisplacement,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::Warp) {
            const auto* params = std::get_if<me::WarpEffectParams>(&fx.params);
            if (!params) continue;

            /* Encode control points as ";"-delimited
             * "src_x,src_y,dst_x,dst_y" entries for the
             * String props slot (matches tone_curve
             * encoding convention). */
            std::string s;
            for (std::size_t i = 0; i < params->control_points.size(); ++i) {
                if (i > 0) s += ';';
                const auto& cp = params->control_points[i];
                s += std::to_string(cp.src_x); s += ',';
                s += std::to_string(cp.src_y); s += ',';
                s += std::to_string(cp.dst_x); s += ',';
                s += std::to_string(cp.dst_y);
            }
            graph::Properties fp;
            fp["control_points"].v = std::move(s);
            auto n = b.add(task::TaskKindId::RenderWarp,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::ChromaticAberration) {
            const auto* params = std::get_if<me::ChromaticAberrationEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["red_dx"].v  = static_cast<int64_t>(params->red_dx);
            fp["red_dy"].v  = static_cast<int64_t>(params->red_dy);
            fp["blue_dx"].v = static_cast<int64_t>(params->blue_dx);
            fp["blue_dy"].v = static_cast<int64_t>(params->blue_dy);
            auto n = b.add(task::TaskKindId::RenderChromaticAberration,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::ScanLines) {
            const auto* params = std::get_if<me::ScanLinesEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["line_height_px"].v  = static_cast<int64_t>(params->line_height_px);
            fp["darkness"].v        = static_cast<double>(params->darkness);
            fp["phase_offset_px"].v = static_cast<int64_t>(params->phase_offset_px);
            auto n = b.add(task::TaskKindId::RenderScanLines,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::Glitch) {
            const auto* params = std::get_if<me::GlitchEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["seed"].v                 = static_cast<int64_t>(params->seed);
            fp["intensity"].v            = static_cast<double>(params->intensity);
            fp["block_size_px"].v        = static_cast<int64_t>(params->block_size_px);
            fp["channel_shift_max_px"].v = static_cast<int64_t>(params->channel_shift_max_px);
            auto n = b.add(task::TaskKindId::RenderGlitch,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::FilmGrain) {
            const auto* params = std::get_if<me::FilmGrainEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            /* Cast uint64 → int64. Negative int64 values wrap
             * to large uint64 in the kernel via two's
             * complement; PRNG seed semantics are preserved. */
            fp["seed"].v          = static_cast<int64_t>(params->seed);
            fp["amount"].v        = static_cast<double>(params->amount);
            fp["grain_size_px"].v = static_cast<int64_t>(params->grain_size_px);
            auto n = b.add(task::TaskKindId::RenderFilmGrain,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::Vignette) {
            const auto* params = std::get_if<me::VignetteEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["radius"].v    = static_cast<double>(params->radius);
            fp["softness"].v  = static_cast<double>(params->softness);
            fp["intensity"].v = static_cast<double>(params->intensity);
            fp["center_x"].v  = static_cast<double>(params->center_x);
            fp["center_y"].v  = static_cast<double>(params->center_y);
            auto n = b.add(task::TaskKindId::RenderVignette,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::HueSaturation) {
            const auto* params = std::get_if<me::HueSaturationEffectParams>(&fx.params);
            if (!params) continue;

            graph::Properties fp;
            fp["hue_shift_deg"].v    = static_cast<double>(params->hue_shift_deg);
            fp["saturation_scale"].v = static_cast<double>(params->saturation_scale);
            fp["lightness_scale"].v  = static_cast<double>(params->lightness_scale);
            auto n = b.add(task::TaskKindId::RenderHueSaturation,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::ToneCurve) {
            const auto* params = std::get_if<me::ToneCurveEffectParams>(&fx.params);
            if (!params) continue;

            /* Encode each per-channel control-point list as
             * "x0,y0;x1,y1;..." for the graph::Properties
             * scalar-string slot. Empty channel curve →
             * empty string → kernel treats as identity. */
            auto encode = [](const std::vector<me::ToneCurvePoint>& pts) {
                std::string s;
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    if (i > 0) s += ';';
                    s += std::to_string(static_cast<int>(pts[i].x));
                    s += ',';
                    s += std::to_string(static_cast<int>(pts[i].y));
                }
                return s;
            };

            graph::Properties fp;
            fp["tone_curve_r_points"].v = encode(params->r);
            fp["tone_curve_g_points"].v = encode(params->g);
            fp["tone_curve_b_points"].v = encode(params->b);
            auto n = b.add(task::TaskKindId::RenderToneCurve,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        } else if (fx.kind == me::EffectKind::FaceMosaic) {
            const auto* params = std::get_if<me::FaceMosaicEffectParams>(&fx.params);
            if (!params) continue;
            std::string landmark_uri;
            auto it = tl.assets.find(params->landmark.asset_id);
            if (it != tl.assets.end()) landmark_uri = it->second.uri;

            graph::Properties fp;
            fp["landmark_asset_uri"].v = landmark_uri;
            fp["frame_t_num"].v        = static_cast<int64_t>(time.num);
            fp["frame_t_den"].v        = static_cast<int64_t>(time.den);
            fp["block_size_px"].v      = static_cast<int64_t>(params->block_size_px);
            fp["mosaic_kind"].v        = static_cast<int64_t>(
                params->kind == me::FaceMosaicEffectParams::Kind::Blur ? 1 : 0);
            auto n = b.add(task::TaskKindId::RenderFaceMosaic,
                            std::move(fp),
                            { prev });
            prev = graph::PortRef{n, 0};
        }
        /* Unknown / unregistered effect kinds fall through —
         * silently skipped at render. New EffectKind values
         * land here as additional `else if` arms. */
    }
    return prev;
}

}  // namespace me::orchestrator::detail
