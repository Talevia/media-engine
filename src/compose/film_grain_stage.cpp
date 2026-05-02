/* film_grain_stage impl. See header for the contract. */
#include "compose/film_grain_stage.hpp"

#include "compose/film_grain_kernel.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace me::compose {

namespace {

double read_double_prop(const graph::Properties& props,
                         const std::string&       key,
                         double                   fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<double>(&it->second.v)) return *p;
    return fallback;
}

std::int64_t read_int64_prop(const graph::Properties& props,
                              const std::string&       key,
                              std::int64_t             fallback) {
    auto it = props.find(key);
    if (it == props.end()) return fallback;
    if (const auto* p = std::get_if<std::int64_t>(&it->second.v)) return *p;
    return fallback;
}

me_status_t film_grain_kernel(task::TaskContext&,
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

    me::FilmGrainEffectParams p;
    /* Cast Int64 → uint64. Negative seeds are valid (they wrap
     * to large positive uint64 values via two's complement) —
     * the user gets a stable seed regardless of int64 sign. */
    p.seed          = static_cast<std::uint64_t>(read_int64_prop(props, "seed", 0));
    p.amount        = static_cast<float>(read_double_prop(props, "amount", 0.0));
    p.grain_size_px = static_cast<int>(read_int64_prop(props, "grain_size_px", 1));

    auto dst = std::make_shared<graph::RgbaFrameData>();
    dst->width  = in.width;
    dst->height = in.height;
    dst->stride = in.stride;
    dst->rgba   = in.rgba;  /* deep copy */

    const me_status_t s = apply_film_grain_inplace(
        dst->rgba.data(), dst->width, dst->height, dst->stride, p);
    if (s != ME_OK) return s;

    outs[0].v = std::move(dst);
    return ME_OK;
}

}  // namespace

void register_film_grain_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderFilmGrain,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = film_grain_kernel,
        .input_schema   = {
            {"in", graph::TypeId::RgbaFrame},
        },
        .output_schema  = { {"out", graph::TypeId::RgbaFrame} },
        .param_schema   = {
            {.name = "seed",          .type = graph::TypeId::Int64},
            {.name = "amount",        .type = graph::TypeId::Float64},
            {.name = "grain_size_px", .type = graph::TypeId::Int64},
        },
    };
    task::register_kind(info);
}

}  // namespace me::compose
