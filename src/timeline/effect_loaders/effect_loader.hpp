/* Per-kind effect-params JSON loaders — extracted from
 * loader_helpers_clip_params.cpp's `parse_effect_spec` switch
 * cascade as the loader-side mirror of cycle 42's
 * `timeline_ir_params.hpp` per-kind sub-header split. Each
 * `parse_<kind>_effect_params` lives in its own
 * `effect_loaders/<kind>.cpp` and validates / converts a JSON
 * params object into the corresponding typed POD.
 *
 * Contract per function:
 *   - `params_json` is the `effect.params` sub-object (already
 *     verified to be an object by the caller).
 *   - `where` is the caller's diagnostic prefix
 *     (e.g. "compositions[0].tracks[0].clips[0].effects[2].params").
 *   - On schema violation throw `LoadError` (caught by the
 *     top-level loader and mapped to ME_E_PARSE / ME_E_UNSUPPORTED).
 *   - Return value is the populated typed-params struct, ready
 *     to assign into `EffectSpec::params` variant.
 *
 * The dispatch from `kind` string to one of these functions
 * lives in `loader_helpers_clip_params.cpp::parse_effect_spec`;
 * each branch is now 2 lines (`spec.kind = …; spec.params = …;`)
 * so adding a new effect kind is: new sub-header + new `.cpp`
 * + 2-line branch in `parse_effect_spec`. */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace me::timeline_loader_detail {

me::ColorEffectParams           parse_color_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::BlurEffectParams            parse_blur_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::LutEffectParams             parse_lut_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::TonemapEffectParams         parse_tonemap_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::InverseTonemapEffectParams  parse_inverse_tonemap_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::FaceStickerEffectParams     parse_face_sticker_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::FaceMosaicEffectParams      parse_face_mosaic_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::BodyAlphaKeyEffectParams    parse_body_alpha_key_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::ToneCurveEffectParams       parse_tone_curve_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::HueSaturationEffectParams   parse_hue_saturation_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::VignetteEffectParams        parse_vignette_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::FilmGrainEffectParams       parse_film_grain_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::GlitchEffectParams          parse_glitch_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::ScanLinesEffectParams       parse_scan_lines_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::ChromaticAberrationEffectParams parse_chromatic_aberration_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::PosterizeEffectParams       parse_posterize_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::OrderedDitherEffectParams   parse_ordered_dither_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::MotionBlurEffectParams      parse_motion_blur_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::RadialBlurEffectParams      parse_radial_blur_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::TiltShiftEffectParams       parse_tilt_shift_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::WarpEffectParams            parse_warp_effect_params(
    const nlohmann::json& params_json, const std::string& where);

me::DisplacementEffectParams    parse_displacement_effect_params(
    const nlohmann::json& params_json, const std::string& where);

}  // namespace me::timeline_loader_detail
