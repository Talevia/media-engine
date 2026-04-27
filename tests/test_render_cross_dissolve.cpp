/*
 * test_render_cross_dissolve — kernel-level cover for the
 * RenderCrossDissolve graph node.
 *
 * Pixel-level lerp math is already covered by test_compose_cross_dissolve;
 * this file pins the kernel ABI:
 *   1. progress=0 → output bytes-identical to `from`.
 *   2. progress=1 → output bytes-identical to `to`.
 *   3. progress=0.5 → halfway lerp per channel.
 *   4. Mismatched dimensions return ME_E_INVALID_ARG.
 *   5. Missing `progress` prop returns ME_E_INVALID_ARG.
 *   6. Schema introspection.
 */
#include <doctest/doctest.h>

#include "compose/cross_dissolve_kernel.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <mutex>
#include <span>
#include <vector>

using namespace me;

namespace {

void register_once() {
    static std::once_flag once;
    std::call_once(once, []() { compose::register_cross_dissolve_kind(); });
}

std::shared_ptr<graph::RgbaFrameData> solid(int w, int h,
                                             uint8_t r, uint8_t g, uint8_t b,
                                             uint8_t a) {
    auto f = std::make_shared<graph::RgbaFrameData>();
    f->width  = w;
    f->height = h;
    f->stride = static_cast<std::size_t>(w) * 4u;
    f->rgba.assign(f->stride * static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4u;
            f->rgba[p + 0] = r;
            f->rgba[p + 1] = g;
            f->rgba[p + 2] = b;
            f->rgba[p + 3] = a;
        }
    }
    return f;
}

me_status_t run_kernel(const graph::Properties& props,
                       const std::vector<graph::InputValue>& ins,
                       graph::OutputSlot& out) {
    auto fn = task::best_kernel_for(task::TaskKindId::RenderCrossDissolve,
                                     task::Affinity::Cpu);
    REQUIRE(fn != nullptr);
    task::TaskContext ctx{};
    return fn(ctx, props,
              std::span<const graph::InputValue>{ins.data(), ins.size()},
              std::span<graph::OutputSlot>      {&out, 1});
}

}  // namespace

TEST_CASE("RenderCrossDissolve: progress=0 → output == from") {
    register_once();

    constexpr int W = 2, H = 2;
    std::vector<graph::InputValue> ins(2);
    ins[0].v = solid(W, H, 200, 100, 50, 255);
    ins[1].v = solid(W, H, 50, 150, 250, 255);

    graph::Properties props;
    props["progress"].v = double(0);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, ins, out) == ME_OK);
    auto& dst = **std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
    CHECK(dst.rgba[0] == 200);
    CHECK(dst.rgba[1] == 100);
    CHECK(dst.rgba[2] == 50);
    CHECK(dst.rgba[3] == 255);
}

TEST_CASE("RenderCrossDissolve: progress=1 → output == to") {
    register_once();

    constexpr int W = 2, H = 2;
    std::vector<graph::InputValue> ins(2);
    ins[0].v = solid(W, H, 200, 100, 50, 255);
    ins[1].v = solid(W, H, 50, 150, 250, 255);

    graph::Properties props;
    props["progress"].v = double(1);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, ins, out) == ME_OK);
    auto& dst = **std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
    CHECK(dst.rgba[0] == 50);
    CHECK(dst.rgba[1] == 150);
    CHECK(dst.rgba[2] == 250);
    CHECK(dst.rgba[3] == 255);
}

TEST_CASE("RenderCrossDissolve: progress=0.5 → midpoint lerp") {
    register_once();

    constexpr int W = 2, H = 2;
    std::vector<graph::InputValue> ins(2);
    ins[0].v = solid(W, H, 0,   0,   0,   255);
    ins[1].v = solid(W, H, 200, 100, 50,  255);

    graph::Properties props;
    props["progress"].v = double(0.5);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, ins, out) == ME_OK);
    auto& dst = **std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
    CHECK(dst.rgba[0] == 100);
    CHECK(dst.rgba[1] == 50);
    CHECK(dst.rgba[2] == 25);
    CHECK(dst.rgba[3] == 255);
}

TEST_CASE("RenderCrossDissolve: dimension mismatch → ME_E_INVALID_ARG") {
    register_once();

    std::vector<graph::InputValue> ins(2);
    ins[0].v = solid(2, 2, 0, 0, 0, 255);
    ins[1].v = solid(4, 4, 0, 0, 0, 255);

    graph::Properties props;
    props["progress"].v = double(0.5);

    graph::OutputSlot out;
    CHECK(run_kernel(props, ins, out) == ME_E_INVALID_ARG);
}

TEST_CASE("RenderCrossDissolve: missing progress → ME_E_INVALID_ARG") {
    register_once();

    std::vector<graph::InputValue> ins(2);
    ins[0].v = solid(2, 2, 0, 0, 0, 255);
    ins[1].v = solid(2, 2, 0, 0, 0, 255);

    graph::Properties props;
    /* progress omitted */

    graph::OutputSlot out;
    CHECK(run_kernel(props, ins, out) == ME_E_INVALID_ARG);
}

TEST_CASE("RenderCrossDissolve: schema declares 2 inputs + 1 output") {
    register_once();
    const auto* info = task::lookup(task::TaskKindId::RenderCrossDissolve);
    REQUIRE(info);
    REQUIRE(info->input_schema.size() == 2);
    CHECK(info->input_schema[0].name == "from");
    CHECK(info->input_schema[1].name == "to");
    CHECK(info->input_schema[0].type == graph::TypeId::RgbaFrame);
    CHECK(info->input_schema[1].type == graph::TypeId::RgbaFrame);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->time_invariant == true);
    CHECK(info->variadic_last_input == false);
}
