/* tone_curve_stage impl. See header for the contract.
 *
 * Properties encode the per-channel control points as
 * "x0,y0;x1,y1;..." strings. The kernel parses them into
 * `me::ToneCurveEffectParams` once per invocation; the cost is
 * amortized against the LUT build + per-pixel pass.
 *
 * Encoding choice rationale: graph::Properties supports only
 * scalar variants (int64, double, string); arrays of pairs
 * require flattening. ASCII pairs keep the encoding human-
 * readable for debug-by-grep + avoid endianness concerns of
 * a byte-packed alternative.
 */
#include "compose/tone_curve_stage.hpp"

#include "compose/tone_curve_kernel.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <charconv>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

/* Parse "x0,y0;x1,y1;..." into a vector of ToneCurvePoints.
 * Returns empty on malformed input — the loader guards against
 * malformed JSON before getting here, so a failure at parse
 * time means an internal serialization bug. */
std::vector<me::ToneCurvePoint> parse_points(std::string_view s) {
    std::vector<me::ToneCurvePoint> out;
    if (s.empty()) return out;
    std::size_t cursor = 0;
    while (cursor < s.size()) {
        const auto comma = s.find(',', cursor);
        if (comma == std::string_view::npos) return {};
        const auto semi = s.find(';', comma);
        const auto x_end = comma;
        const auto y_end = (semi == std::string_view::npos) ? s.size() : semi;
        int xv = 0, yv = 0;
        const auto x_sv = s.substr(cursor, x_end - cursor);
        const auto y_sv = s.substr(comma + 1, y_end - (comma + 1));
        if (std::from_chars(x_sv.data(), x_sv.data() + x_sv.size(), xv).ec
                != std::errc{}) return {};
        if (std::from_chars(y_sv.data(), y_sv.data() + y_sv.size(), yv).ec
                != std::errc{}) return {};
        if (xv < 0 || xv > 255 || yv < 0 || yv > 255) return {};
        out.push_back({static_cast<std::uint8_t>(xv),
                        static_cast<std::uint8_t>(yv)});
        if (semi == std::string_view::npos) break;
        cursor = semi + 1;
    }
    return out;
}

me_status_t tone_curve_kernel(task::TaskContext&,
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

    me::ToneCurveEffectParams p;
    p.r = parse_points(read_string_prop(props, "tone_curve_r_points"));
    p.g = parse_points(read_string_prop(props, "tone_curve_g_points"));
    p.b = parse_points(read_string_prop(props, "tone_curve_b_points"));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy */

    const me_status_t s = apply_tone_curve_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_tone_curve_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderToneCurve,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,  /* pure pixel transform; no time dep. */
        .kernel         = tone_curve_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "tone_curve_r_points", .type = graph::TypeId::String},
            {.name = "tone_curve_g_points", .type = graph::TypeId::String},
            {.name = "tone_curve_b_points", .type = graph::TypeId::String},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
