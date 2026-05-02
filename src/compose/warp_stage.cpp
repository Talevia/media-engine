/* warp_stage impl. Decodes the ;-delimited "src_x,src_y,dst_x,dst_y"
 * control-point string from graph::Properties (matches tone_curve
 * encoding convention). */
#include "compose/warp_stage.hpp"

#include "compose/warp_kernel.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace me::compose {

namespace {

std::string read_string_prop(const graph::Properties& props,
                              const std::string&       key) {
    auto it = props.find(key);
    if (it == props.end()) return {};
    if (const auto* p = std::get_if<std::string>(&it->second.v)) return *p;
    return {};
}

/* Parse "sx0,sy0,dx0,dy0;sx1,sy1,dx1,dy1;..." into control points.
 * Empty input → empty vector. Malformed entries → caller relies on
 * loader to have validated; we silently drop malformed records. */
std::vector<me::WarpControlPoint> decode_control_points(
    const std::string& s) {
    std::vector<me::WarpControlPoint> out;
    if (s.empty()) return out;

    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t end = s.find(';', i);
        if (end == std::string::npos) end = s.size();
        const std::string entry = s.substr(i, end - i);
        if (!entry.empty()) {
            float v[4] = {0.f, 0.f, 0.f, 0.f};
            int   nv   = 0;
            std::size_t j = 0;
            while (j < entry.size() && nv < 4) {
                std::size_t k = entry.find(',', j);
                if (k == std::string::npos) k = entry.size();
                v[nv++] = static_cast<float>(
                    std::strtod(entry.substr(j, k - j).c_str(), nullptr));
                j = (k == entry.size()) ? k : k + 1;
            }
            if (nv == 4) {
                me::WarpControlPoint cp;
                cp.src_x = v[0]; cp.src_y = v[1];
                cp.dst_x = v[2]; cp.dst_y = v[3];
                out.push_back(cp);
            }
        }
        i = (end == s.size()) ? end : end + 1;
    }
    return out;
}

me_status_t warp_kernel_fn(task::TaskContext&,
                            const graph::Properties&           props,
                            std::span<const graph::InputValue> inputs,
                            std::span<graph::OutputSlot>       outs) {
    if (inputs.size() < 1) return ME_E_INVALID_ARG;
    if (outs.size()   < 1) return ME_E_INVALID_ARG;

    auto* in_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!in_pp || !*in_pp) return ME_E_INVALID_ARG;
    const auto& in = **in_pp;
    if (in.width <= 0 || in.height <= 0 ||
        in.stride < static_cast<std::size_t>(in.width) * 4) {
        return ME_E_INVALID_ARG;
    }

    me::WarpEffectParams p;
    p.control_points = decode_control_points(
        read_string_prop(props, "control_points"));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;

    const me_status_t s = apply_warp_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_warp_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderWarp,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = warp_kernel_fn,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "control_points", .type = graph::TypeId::String},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
