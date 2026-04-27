#include "compose/compose_cpu_kernel.hpp"

#include "compose/alpha_over.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::compose {

namespace {

bool prop_int(const graph::Properties& props,
              const std::string&       key,
              int64_t&                 out) {
    auto it = props.find(key);
    if (it == props.end()) return false;
    if (auto* p = std::get_if<int64_t>(&it->second.v)) {
        out = *p;
        return true;
    }
    return false;
}

double prop_double_or(const graph::Properties& props,
                      const std::string&       key,
                      double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

int64_t prop_int_or(const graph::Properties& props,
                    const std::string&       key,
                    int64_t                  fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (auto* p = std::get_if<int64_t>(&it->second.v)) return *p;
    return fallback;
}

BlendMode blend_mode_from_int(int64_t v) {
    switch (v) {
    case 1: return BlendMode::Multiply;
    case 2: return BlendMode::Screen;
    default: return BlendMode::Normal;
    }
}

me_status_t compose_cpu_kernel(task::TaskContext&,
                                const graph::Properties&           props,
                                std::span<const graph::InputValue> inputs,
                                std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;

    int64_t dst_w_i64 = 0, dst_h_i64 = 0;
    if (!prop_int(props, "dst_w", dst_w_i64) ||
        !prop_int(props, "dst_h", dst_h_i64)) {
        return ME_E_INVALID_ARG;
    }
    if (dst_w_i64 <= 0 || dst_h_i64 <= 0) return ME_E_INVALID_ARG;
    const int dst_w = static_cast<int>(dst_w_i64);
    const int dst_h = static_cast<int>(dst_h_i64);

    auto out_frame = std::make_shared<graph::RgbaFrameData>();
    out_frame->width  = dst_w;
    out_frame->height = dst_h;
    out_frame->stride = static_cast<std::size_t>(dst_w) * 4u;
    out_frame->rgba.assign(out_frame->stride * static_cast<std::size_t>(dst_h), 0);

    /* Composite layer-by-layer onto the (initially transparent) canvas.
     * Every layer goes through alpha_over so each one carries its own
     * opacity + blend_mode; the bottom layer over transparent black
     * with Normal mode collapses to a straight copy. */
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto* layer_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[i].v);
        if (!layer_pp || !*layer_pp) return ME_E_INVALID_ARG;
        const auto& layer = **layer_pp;
        if (layer.width != dst_w || layer.height != dst_h) return ME_E_INVALID_ARG;
        if (layer.stride != out_frame->stride) return ME_E_INVALID_ARG;

        const std::string idx = std::to_string(i);
        const float    opacity   = static_cast<float>(
            prop_double_or(props, "opacity_" + idx, 1.0));
        const BlendMode mode = blend_mode_from_int(
            prop_int_or(props, "blend_mode_" + idx, 0));

        alpha_over(out_frame->rgba.data(),
                   layer.rgba.data(),
                   dst_w, dst_h, out_frame->stride,
                   opacity, mode);
    }

    outs[0].v = std::move(out_frame);
    return ME_OK;
}

}  // namespace

void register_compose_cpu_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderComposeCpu,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .variadic_last_input = true,
        .kernel         = compose_cpu_kernel,
        /* Variadic input — input_schema's lone "layer" entry describes
         * every actual port (Builder accepts ≥1 layers when
         * variadic_last_input=true). */
        .input_schema   = { {"layer", graph::TypeId::RgbaFrame} },
        .output_schema  = { {"composite", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "dst_w", .type = graph::TypeId::Int64},
            {.name = "dst_h", .type = graph::TypeId::Int64},
            /* Per-layer opacity_<i> / blend_mode_<i> are runtime keys;
             * declaring them in param_schema is impossible without
             * variadic schema entries. Builder doesn't enforce the
             * runtime keys, only the declared ones. */
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
