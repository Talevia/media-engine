/* Low-level timeline IR parameter types — ColorSpace + Transform +
 * clip-level parameter POD structs + Effect variant umbrella.
 *
 * Scope: types referenced by `Clip` / `Asset` / `Timeline` but not
 * by one another (outside this header). Split off
 * `timeline_impl.hpp` as part of `debt-split-timeline-impl-hpp` —
 * the main composition-structure header was 433 lines before;
 * splitting parameters here drops it under the 400-line threshold
 * while keeping each definition at its documentation. Included
 * transitively via `timeline_impl.hpp`; TUs that only need the
 * params (e.g. loader_helpers parsers) may include this header
 * directly.
 *
 * Effect-param sub-headers (debt-split-timeline-ir-params).
 * Each typed-effect-param struct lives in its own header under
 * `timeline/effect_params/`; this umbrella re-includes them so
 * existing call sites (`#include "timeline/timeline_ir_params.hpp"`)
 * continue to see the full set. The `EffectKind` enum + the
 * `EffectSpec::params` `std::variant` alias stay here because
 * the variant-index ↔ EffectKind ordering invariant is more
 * locally maintainable when the variant is one declaration that
 * names every member. New effect kinds add a sub-header + an
 * include here + an enum entry + a variant slot — same blast
 * radius, smaller per-cycle TU edits.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/animated_color.hpp"
#include "timeline/animated_number.hpp"
#include "timeline/effect_params/blur.hpp"
#include "timeline/effect_params/body_alpha_key.hpp"
#include "timeline/effect_params/chromatic_aberration.hpp"
#include "timeline/effect_params/color.hpp"
#include "timeline/effect_params/face_mosaic.hpp"
#include "timeline/effect_params/face_sticker.hpp"
#include "timeline/effect_params/film_grain.hpp"
#include "timeline/effect_params/glitch.hpp"
#include "timeline/effect_params/hue_saturation.hpp"
#include "timeline/effect_params/inverse_tonemap.hpp"
#include "timeline/effect_params/lut.hpp"
#include "timeline/effect_params/motion_blur.hpp"
#include "timeline/effect_params/ordered_dither.hpp"
#include "timeline/effect_params/radial_blur.hpp"
#include "timeline/effect_params/posterize.hpp"
#include "timeline/effect_params/scan_lines.hpp"
#include "timeline/effect_params/tone_curve.hpp"
#include "timeline/effect_params/tonemap.hpp"
#include "timeline/effect_params/vignette.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace me {

/* Per-asset color space override (TIMELINE_SCHEMA.md §Color). Each axis is
 * independently optional — `Unspecified` means "trust container metadata
 * for this axis". Populated into Asset only when JSON explicitly carries
 * `"colorSpace":{...}`; a missing object leaves Asset::color_space as
 * std::nullopt (= trust container for everything).
 *
 * M2 OCIO pipeline consumes these enums to pick a working-space transform.
 * Keep the enumeration stable with the schema string tables in the loader;
 * adding a value needs a matching entry in both directions. */
struct ColorSpace {
    enum class Primaries : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020, P3_D65
    };
    enum class Transfer : uint8_t {
        Unspecified = 0, BT709, SRGB, Linear, PQ, HLG, Gamma22, Gamma28
    };
    enum class Matrix : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020NC, Identity
    };
    enum class Range : uint8_t {
        Unspecified = 0, Limited, Full
    };

    Primaries primaries{Primaries::Unspecified};
    Transfer  transfer {Transfer::Unspecified};
    Matrix    matrix   {Matrix::Unspecified};
    Range     range    {Range::Unspecified};
};

/* Snapshot of a Transform's 8 fields at a specific composition time.
 * Produced by `Transform::evaluate_at(t)`; consumed by the compose
 * loop / affine math. Identity defaults match Transform's defaults. */
struct TransformEvaluated {
    double translate_x  = 0.0;
    double translate_y  = 0.0;
    double scale_x      = 1.0;
    double scale_y      = 1.0;
    double rotation_deg = 0.0;
    double opacity      = 1.0;
    double anchor_x     = 0.5;
    double anchor_y     = 0.5;

    /* Spatial identity ⇔ translate == 0, scale == 1, rotation == 0.
     * Opacity and anchor don't participate — opacity is applied via
     * alpha_over (not spatial); anchor only matters when there's
     * rotation/scale. */
    bool spatial_identity() const {
        return translate_x  == 0.0 &&
               translate_y  == 0.0 &&
               scale_x      == 1.0 &&
               scale_y      == 1.0 &&
               rotation_deg == 0.0;
    }
};

/* 2D transform applied when the clip composites onto the output canvas.
 * Each field is an `AnimatedNumber` — supports `{"static": v}` and
 * `{"keyframes": [...]}` JSON forms (migrated by the
 * `transform-animated-support` bullet layer 3). Identity defaults make
 * `Transform{}` a valid "no-op" state.
 *
 * Caller pattern: `auto eval = clip.transform->evaluate_at(T);` — reads
 * 8 doubles into a `TransformEvaluated` at composition time T. Callers
 * that ignore T (e.g. preview-at-t=0 flat static) can pass
 * `me_rational_t{0, 1}`.
 *
 * Stored as std::optional<Transform> on Clip: nullopt = "JSON clip has
 * no `transform` key" (vs Transform{} = "transform key present with
 * all identity defaults"). The distinction lets downstream code
 * fast-path clips that truly omit transforms. */
struct Transform {
    AnimatedNumber translate_x  = AnimatedNumber::from_static(0.0);
    AnimatedNumber translate_y  = AnimatedNumber::from_static(0.0);
    AnimatedNumber scale_x      = AnimatedNumber::from_static(1.0);
    AnimatedNumber scale_y      = AnimatedNumber::from_static(1.0);
    AnimatedNumber rotation_deg = AnimatedNumber::from_static(0.0);
    AnimatedNumber opacity      = AnimatedNumber::from_static(1.0);
    AnimatedNumber anchor_x     = AnimatedNumber::from_static(0.5);
    AnimatedNumber anchor_y     = AnimatedNumber::from_static(0.5);

    TransformEvaluated evaluate_at(me_rational_t t) const {
        return TransformEvaluated{
            translate_x.evaluate_at(t),
            translate_y.evaluate_at(t),
            scale_x.evaluate_at(t),
            scale_y.evaluate_at(t),
            rotation_deg.evaluate_at(t),
            opacity.evaluate_at(t),
            anchor_x.evaluate_at(t),
            anchor_y.evaluate_at(t),
        };
    }
};

/* Media kind of a clip. Mirrors TIMELINE_SCHEMA.md §Clip `"type"` enum
 * values "video" / "audio" / "text" / "subtitle". Loader enforces
 * that a clip's type matches its parent track's kind (no cross-kind
 * mixing). Enum values are ABI-stable once shipped — append new
 * kinds, never reorder. */
enum class ClipType : uint8_t {
    Video    = 0,
    Audio    = 1,
    Text     = 2,
    Subtitle = 3,
};

/* Synthetic text-clip parameters — used when ClipType::Text. Has no
 * source asset (no decoder, no demux); the renderer draws directly
 * from these fields per output frame.
 *
 * `content` is the UTF-8 string to render. Empty is valid (renders
 * nothing but still takes space on the timeline).
 *
 * `font_size`, `x`, `y` are AnimatedNumbers so each can keyframe
 * independently (size pulse, position tween). Defaults are
 * reasonable static values — 48 pixel font at (0, 0).
 *
 * `color` is a CSS-like hex string ("#RRGGBB" / "#RRGGBBAA"). Loader
 * validates the string shape; the renderer converts to float-RGBA
 * at draw time. Static string (not AnimatedString) — color
 * animation would use a typed animated-color primitive if / when a
 * consumer needs it.
 *
 * `font_family` is an optional font family name ("Helvetica",
 * "Noto Sans SC", "Apple Color Emoji"). Empty = platform default.
 * Renderer's font resolver (future: CoreText on macOS, fontconfig
 * on Linux) walks fallbacks for characters the primary font lacks.
 */
struct TextClipParams {
    std::string    content;
    /* Animated RGBA. Default = opaque white. JSON accepts three
     * shapes (legacy / explicit static / keyframed) — see
     * animated_color.hpp doc. */
    AnimatedColor  color      = AnimatedColor::default_opaque_white();
    std::string    font_family;
    AnimatedNumber font_size  = AnimatedNumber::from_static(48.0);
    AnimatedNumber x          = AnimatedNumber::from_static(0.0);
    AnimatedNumber y          = AnimatedNumber::from_static(0.0);

    /* Paragraph layout — absent / nullopt = old single-line
     * behaviour (draw_string_with_fallback, no wrap). Positive
     * value activates word-wrap via SkiaBackend::draw_paragraph:
     * the renderer greedy-breaks the content at codepoint
     * boundaries so no line exceeds max_width pixels, and
     * advances the y cursor by `font_size *
     * line_height_multiplier` between lines. Explicit `\n` in
     * content always starts a new line regardless of max_width. */
    std::optional<double> max_width;
    double                line_height_multiplier = 1.2;
};

/* Synthetic subtitle-clip parameters — used when ClipType::Subtitle.
 * Has no source asset; the SubtitleRenderer parses the inline
 * `content` string OR the file referenced by `file_uri` once at
 * clip entry and rasterizes per-frame glyphs via libass.
 *
 * The loader enforces "exactly one of {content, file_uri}" —
 * supplying neither or both is ME_E_PARSE. file_uri follows the
 * same URI conventions as Asset::uri (file://… supported; the
 * compose-loop strips the prefix and reads via std::ifstream).
 * Inline content is self-contained and keeps the timeline JSON
 * portable; file_uri is the path for larger subtitle files that
 * would bloat the JSON. */
struct SubtitleClipParams {
    std::string content;    /* inline .ass / .srt text (mutex with file_uri) */
    std::string file_uri;   /* file:// URI to a .ass / .srt file (mutex with content) */
    std::string codepage;   /* optional: passed to libass for non-UTF-8 */
};

/* Typed effect parameter tagged union.
 *
 * VISION §3.2 forbids `Map<String, Float>`-shaped effect parameter
 * APIs. Each EffectKind has its own POD parameter struct living in
 * its own sub-header (see top-of-file include block); EffectSpec
 * holds a std::variant over them so add-a-new-kind is a variant
 * extension + a sub-header + a parse branch, not a map entry.
 *
 * Params are plain numbers (not AnimatedNumber) today — keyframed
 * effect params arrive with the `effect-param-animated` cycle once
 * GPU effects actually consume them. Schema doc lists all three
 * kinds' param names (TIMELINE_SCHEMA.md §Effect "Core kinds").
 *
 * Ranges documented here are *semantic* and not loader-enforced —
 * downstream GPU effects clamp to their shader's valid domain.
 * Loader only enforces "required params present, types correct". */

/* EffectKind enum. Stable once shipped — appending new kinds is ABI-
 * safe (new enum value + new variant alternative); reordering /
 * removing kinds is not. JSON tags ("color", "blur", "lut",
 * "tonemap", "inverse_tonemap", "face_sticker", "face_mosaic",
 * "body_alpha_key") live in loader_helpers.cpp's dispatch; add
 * entries in lock-step. */
enum class EffectKind : uint8_t {
    Color           = 0,
    Blur            = 1,
    Lut             = 2,
    Tonemap         = 3,
    InverseTonemap  = 4,
    FaceSticker     = 5,
    FaceMosaic      = 6,
    BodyAlphaKey    = 7,
    ToneCurve       = 8,
    HueSaturation   = 9,
    Vignette        = 10,
    FilmGrain       = 11,
    Glitch          = 12,
    ScanLines       = 13,
    ChromaticAberration = 14,
    Posterize       = 15,
    OrderedDither   = 16,
    MotionBlur      = 17,
    RadialBlur      = 18,
};

struct EffectSpec {
    /* Optional JSON "id" for addressable effect updates (future M3+
     * scrub-time parameter tweaks). Empty when JSON omits "id". */
    std::string    id;

    EffectKind     kind{EffectKind::Color};

    /* `enabled` defaults true per TIMELINE_SCHEMA.md §Effect. Consumer
     * skips disabled effects entirely — cheaper than running through
     * `mix=0`. */
    bool           enabled{true};

    /* Blend factor between input and effect output. AnimatedNumber so
     * keyframed fades work on the same `{"static": v}` / `{"keyframes":
     * [...]}` shape as Transform fields. Default = full effect. */
    AnimatedNumber mix = AnimatedNumber::from_static(1.0);

    /* Typed params by EffectKind. The variant's index must match the
     * kind enum's underlying value (Color → 0, Blur → 1, Lut → 2,
     * Tonemap → 3, InverseTonemap → 4, FaceSticker → 5, FaceMosaic →
     * 6, BodyAlphaKey → 7, ToneCurve → 8, HueSaturation → 9,
     * Vignette → 10, FilmGrain → 11, Glitch → 12,
     * ScanLines → 13, ChromaticAberration → 14, Posterize →
     * 15, OrderedDither → 16, MotionBlur → 17,
     * RadialBlur → 18) so consumers can
     * `std::get_if<ColorEffectParams>(&spec.params)` without
     * re-checking kind. Loader enforces the invariant. */
    std::variant<ColorEffectParams, BlurEffectParams, LutEffectParams,
                 TonemapEffectParams, InverseTonemapEffectParams,
                 FaceStickerEffectParams, FaceMosaicEffectParams,
                 BodyAlphaKeyEffectParams, ToneCurveEffectParams,
                 HueSaturationEffectParams, VignetteEffectParams,
                 FilmGrainEffectParams, GlitchEffectParams,
                 ScanLinesEffectParams,
                 ChromaticAberrationEffectParams,
                 PosterizeEffectParams,
                 OrderedDitherEffectParams,
                 MotionBlurEffectParams,
                 RadialBlurEffectParams>
        params{ColorEffectParams{}};
};

}  // namespace me
