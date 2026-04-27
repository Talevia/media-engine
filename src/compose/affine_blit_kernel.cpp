#include "compose/affine_blit_kernel.hpp"

#include "compose/affine_blit.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <span>
#include <utility>

namespace me::compose {

namespace {

double prop_double(const graph::Properties& props,
                   const char*              key,
                   double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

bool prop_int(const graph::Properties& props,
              const char*              key,
              int64_t&                 out) {
    auto it = props.find(key);
    if (it == props.end()) return false;
    if (auto* p = std::get_if<int64_t>(&it->second.v)) {
        out = *p;
        return true;
    }
    return false;
}

me_status_t affine_blit_kernel(task::TaskContext&,
                               const graph::Properties&           props,
                               std::span<const graph::InputValue> inputs,
                               std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* src_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!src_pp || !*src_pp) return ME_E_INVALID_ARG;
    const auto& src = **src_pp;
    if (src.width <= 0 || src.height <= 0 || src.stride == 0) return ME_E_INVALID_ARG;

    int64_t dst_w_i64 = 0;
    int64_t dst_h_i64 = 0;
    if (!prop_int(props, "dst_w", dst_w_i64) ||
        !prop_int(props, "dst_h", dst_h_i64)) {
        return ME_E_INVALID_ARG;
    }
    if (dst_w_i64 <= 0 || dst_h_i64 <= 0) return ME_E_INVALID_ARG;
    const int dst_w = static_cast<int>(dst_w_i64);
    const int dst_h = static_cast<int>(dst_h_i64);

    const double translate_x  = prop_double(props, "translate_x",  0.0);
    const double translate_y  = prop_double(props, "translate_y",  0.0);
    const double scale_x      = prop_double(props, "scale_x",      1.0);
    const double scale_y      = prop_double(props, "scale_y",      1.0);
    const double rotation_deg = prop_double(props, "rotation_deg", 0.0);
    const double anchor_x     = prop_double(props, "anchor_x",     0.0);
    const double anchor_y     = prop_double(props, "anchor_y",     0.0);

    const AffineMatrix inv = compose_inverse_affine(
        translate_x, translate_y,
        scale_x, scale_y,
        rotation_deg,
        anchor_x, anchor_y,
        src.width, src.height);

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = dst_w;
    dst->height = dst_h;
    dst->stride = static_cast<std::size_t>(dst_w) * 4u;
    dst->rgba.assign(dst->stride * static_cast<std::size_t>(dst_h), 0);

    affine_blit(dst->rgba.data(), dst_w, dst_h, dst->stride,
                src.rgba.data(),  src.width, src.height, src.stride,
                inv);

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_affine_blit_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderAffineBlit,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = affine_blit_kernel,
        .input_schema   = { {"src", graph::TypeId::RgbaFrame} },
        .output_schema  = { {"dst", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "dst_w",        .type = graph::TypeId::Int64},
            {.name = "dst_h",        .type = graph::TypeId::Int64},
            {.name = "translate_x",  .type = graph::TypeId::Float64},
            {.name = "translate_y",  .type = graph::TypeId::Float64},
            {.name = "scale_x",      .type = graph::TypeId::Float64},
            {.name = "scale_y",      .type = graph::TypeId::Float64},
            {.name = "rotation_deg", .type = graph::TypeId::Float64},
            {.name = "anchor_x",     .type = graph::TypeId::Float64},
            {.name = "anchor_y",     .type = graph::TypeId::Float64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
