/* face_sticker_stage impl. See header for the contract.
 *
 * Kernel chain inside the call:
 *   1. Read RGBA8 input + properties.
 *   2. decode_sticker_to_rgba8 (PNG → RGBA8 sticker pixels).
 *   3. resolve_landmark_bboxes_from_file (JSON fixture → bbox span).
 *   4. apply_face_sticker_inplace (in-place blend).
 *
 * The kernel allocates a fresh RgbaFrameData for the output and
 * memcpys the input bytes into it before applying the in-place
 * blend — graph values are immutable, so we can't blend into the
 * input buffer.
 */
#include "compose/face_sticker_stage.hpp"

#include "compose/bbox.hpp"
#include "compose/face_sticker_kernel.hpp"
#include "compose/landmark_resolver.hpp"
#include "compose/sticker_decoder.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/effect_params/face_sticker.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::compose {

namespace {

/* Read a `(num, den)` rational pair from properties under the
 * given prefixes. Returns `{0, 1}` on missing or malformed
 * entries. */
me_rational_t read_rational_props(const graph::Properties& props,
                                   const std::string&       num_key,
                                   const std::string&       den_key) {
    me_rational_t out{0, 1};
    auto n_it = props.find(num_key);
    auto d_it = props.find(den_key);
    if (n_it == props.end() || d_it == props.end()) return out;
    const auto* n_p = std::get_if<std::int64_t>(&n_it->second.v);
    const auto* d_p = std::get_if<std::int64_t>(&d_it->second.v);
    if (!n_p || !d_p) return out;
    out.num = *n_p;
    out.den = (*d_p > 0) ? *d_p : 1;
    return out;
}

double read_double_prop(const graph::Properties& props,
                         const std::string&       key,
                         double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

std::string read_string_prop(const graph::Properties& props,
                              const std::string&       key) {
    auto it = props.find(key);
    if (it == props.end()) return {};
    if (const auto* p = std::get_if<std::string>(&it->second.v)) return *p;
    return {};
}

me_status_t face_sticker_kernel(task::TaskContext&                  ctx,
                                 const graph::Properties&           props,
                                 std::span<const graph::InputValue> inputs,
                                 std::span<graph::OutputSlot>       outs) {
    if (inputs.size() < 1) return ME_E_INVALID_ARG;
    if (outs.size()   < 1) return ME_E_INVALID_ARG;

    /* --- Read input frame ------------------------------------- */
    auto* in_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!in_pp || !*in_pp) return ME_E_INVALID_ARG;
    const auto& in = **in_pp;
    if (in.width <= 0 || in.height <= 0 ||
        in.stride < static_cast<std::size_t>(in.width) * 4) {
        return ME_E_INVALID_ARG;
    }

    /* --- Read properties -------------------------------------- */
    const std::string sticker_uri      = read_string_prop(props, "sticker_uri");
    const std::string landmark_uri     = read_string_prop(props, "landmark_asset_uri");
    const me_rational_t frame_t        = read_rational_props(props, "frame_t_num", "frame_t_den");
    const double scale_x               = read_double_prop(props, "scale_x", 1.0);
    const double scale_y               = read_double_prop(props, "scale_y", 1.0);
    const double offset_x              = read_double_prop(props, "offset_x", 0.0);
    const double offset_y              = read_double_prop(props, "offset_y", 0.0);

    /* --- Resolve sticker pixels (PNG decode) ------------------ */
    StickerImage sticker;
    std::string  err;
    if (!sticker_uri.empty()) {
        const me_status_t s = decode_sticker_to_rgba8(sticker_uri, &sticker, &err);
        if (s != ME_OK) {
            /* Sticker decode failure → propagate. The face_sticker
             * kernel handles `sticker_rgba == nullptr` as a no-op
             * but a CONFIGURED sticker that fails to decode is a
             * legitimate render error (likely bad URI), not a
             * silent skip. */
            return s;
        }
    }

    /* --- Resolve landmark bboxes ------------------------------
     * Two URI shapes:
     *   - "model:<id>/<ver>/<quant>" → runtime ML inference via
     *     resolve_landmark_bboxes_runtime (gated by ME_HAS_INFERENCE
     *     + a non-NULL ctx.engine; falls back to empty bboxes on
     *     ME_E_UNSUPPORTED so kernels stay rendering when the
     *     runtime backend isn't compiled in).
     *   - anything else → file-based JSON fixture path. */
    std::vector<Bbox> bboxes;
    if (!landmark_uri.empty()) {
        const bool is_model_uri = landmark_uri.rfind("model:", 0) == 0;
        if (is_model_uri) {
#ifdef ME_HAS_INFERENCE
            if (ctx.engine) {
                const me_status_t s = resolve_landmark_bboxes_runtime(
                    ctx.engine, landmark_uri, frame_t,
                    in.width, in.height,
                    in.rgba.data(), in.stride,
                    &bboxes, &err);
                if (s == ME_E_UNSUPPORTED) {
                    bboxes.clear();  /* no runtime backend → empty */
                } else if (s != ME_OK) {
                    return s;
                }
            }
#endif
        } else {
            const me_status_t s = resolve_landmark_bboxes_from_file(
                landmark_uri, frame_t, &bboxes, &err);
            if (s != ME_OK) return s;
        }
    }

    /* --- Allocate output, copy input bytes, in-place blend ---- */
    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy — graph values are immutable */

    me::FaceStickerEffectParams p;
    p.landmark.asset_id = landmark_uri;  /* documentation-only */
    p.sticker_uri       = sticker_uri;
    p.scale_x           = scale_x;
    p.scale_y           = scale_y;
    p.offset_x          = offset_x;
    p.offset_y          = offset_y;

    const std::uint8_t* sticker_data =
        sticker.pixels.empty() ? nullptr : sticker.pixels.data();
    const std::size_t sticker_stride =
        static_cast<std::size_t>(sticker.width) * 4;

    const me_status_t s = apply_face_sticker_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride,
        p,
        std::span<const Bbox>(bboxes.data(), bboxes.size()),
        sticker_data, sticker.width, sticker.height,
        sticker_stride);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_face_sticker_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderFaceSticker,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        /* Not time_invariant: landmark resolver samples per frame_t
         * so the same input frame at different times can produce
         * different bboxes → different output. */
        .time_invariant = false,
        .kernel         = face_sticker_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "sticker_uri",        .type = graph::TypeId::String},
            {.name = "landmark_asset_uri", .type = graph::TypeId::String},
            {.name = "frame_t_num",        .type = graph::TypeId::Int64},
            {.name = "frame_t_den",        .type = graph::TypeId::Int64},
            {.name = "scale_x",            .type = graph::TypeId::Float64},
            {.name = "scale_y",            .type = graph::TypeId::Float64},
            {.name = "offset_x",           .type = graph::TypeId::Float64},
            {.name = "offset_y",           .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
