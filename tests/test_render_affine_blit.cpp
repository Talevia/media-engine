/*
 * test_render_affine_blit — kernel-level cover for the RenderAffineBlit
 * graph node. Pins:
 *   1. Identity transform produces the same RGBA buffer at the requested
 *      output canvas size (truncates / pads vs. src as needed).
 *   2. Translate shifts pixels by the integer offset.
 *   3. Scale 2× doubles each src pixel into a 2×2 dst block.
 *   4. Missing required dst_w / dst_h returns ME_E_INVALID_ARG.
 *
 * The pixel-level math is already exercised by test_compose_affine_blit
 * (the helper). This file's job is to verify the kernel ABI:
 *   - reads RgbaFrame from input port 0
 *   - reads transform params from graph::Properties
 *   - allocates output RgbaFrame at dst_w × dst_h
 *   - returns ME_OK / proper error codes
 */
#include <doctest/doctest.h>

#include "compose/affine_blit_kernel.hpp"
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
    std::call_once(once, []() { compose::register_affine_blit_kind(); });
}

std::shared_ptr<graph::RgbaFrameData> labeled_rgba(int w, int h) {
    auto f = std::make_shared<graph::RgbaFrameData>();
    f->width  = w;
    f->height = h;
    f->stride = static_cast<std::size_t>(w) * 4u;
    f->rgba.assign(f->stride * static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4u;
            f->rgba[p + 0] = static_cast<uint8_t>(x);
            f->rgba[p + 1] = static_cast<uint8_t>(y);
            f->rgba[p + 2] = 0;
            f->rgba[p + 3] = 255;
        }
    }
    return f;
}

me_status_t run_kernel(const graph::Properties& props,
                       const graph::InputValue& in,
                       graph::OutputSlot&       out) {
    auto fn = task::best_kernel_for(task::TaskKindId::RenderAffineBlit,
                                     task::Affinity::Cpu);
    REQUIRE(fn != nullptr);
    task::TaskContext ctx{};
    return fn(ctx, props,
              std::span<const graph::InputValue>{&in, 1},
              std::span<graph::OutputSlot>      {&out, 1});
}

}  // namespace

TEST_CASE("RenderAffineBlit: identity copies src into same-size dst") {
    register_once();

    constexpr int W = 4, H = 4;
    graph::InputValue in;
    in.v = labeled_rgba(W, H);

    graph::Properties props;
    props["dst_w"].v = int64_t(W);
    props["dst_h"].v = int64_t(H);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, in, out) == ME_OK);

    auto dst_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
    REQUIRE(dst_pp);
    REQUIRE(*dst_pp);
    const auto& dst = **dst_pp;
    CHECK(dst.width  == W);
    CHECK(dst.height == H);
    CHECK(dst.rgba.size() == static_cast<std::size_t>(W * H * 4));
    /* Identity → dst[x,y] == src[x,y]. */
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * W + x) * 4u;
            CHECK(dst.rgba[p + 0] == static_cast<uint8_t>(x));
            CHECK(dst.rgba[p + 1] == static_cast<uint8_t>(y));
            CHECK(dst.rgba[p + 3] == 255);
        }
    }
}

TEST_CASE("RenderAffineBlit: pure translate offsets src in dst") {
    register_once();

    constexpr int W = 8, H = 8;
    graph::InputValue in;
    in.v = labeled_rgba(W, H);

    graph::Properties props;
    props["dst_w"].v = int64_t(W);
    props["dst_h"].v = int64_t(H);
    props["translate_x"].v = double(2);
    props["translate_y"].v = double(1);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, in, out) == ME_OK);
    auto& dst = **std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);

    /* dst (5, 3) ← src (3, 2). */
    const std::size_t p = (3u * W + 5u) * 4u;
    CHECK(dst.rgba[p + 0] == 3);
    CHECK(dst.rgba[p + 1] == 2);
    CHECK(dst.rgba[p + 3] == 255);

    /* dst (0, 0) samples src (-2, -1) → out of bounds → transparent. */
    CHECK(dst.rgba[3] == 0);
}

TEST_CASE("RenderAffineBlit: scale 2× to larger canvas") {
    register_once();

    constexpr int SW = 4, SH = 4;
    graph::InputValue in;
    in.v = labeled_rgba(SW, SH);

    graph::Properties props;
    props["dst_w"].v = int64_t(8);
    props["dst_h"].v = int64_t(8);
    props["scale_x"].v = double(2);
    props["scale_y"].v = double(2);

    graph::OutputSlot out;
    REQUIRE(run_kernel(props, in, out) == ME_OK);
    auto& dst = **std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
    CHECK(dst.width  == 8);
    CHECK(dst.height == 8);

    /* dst (0,0) ← src (0,0); dst (2,2) ← src (1,1). */
    CHECK(dst.rgba[0] == 0);
    const std::size_t p22 = (2u * 8u + 2u) * 4u;
    CHECK(dst.rgba[p22 + 0] == 1);
    CHECK(dst.rgba[p22 + 1] == 1);
}

TEST_CASE("RenderAffineBlit: missing dst_w / dst_h → ME_E_INVALID_ARG") {
    register_once();

    graph::InputValue in;
    in.v = labeled_rgba(2, 2);

    graph::Properties props;
    /* dst_w / dst_h omitted. */
    graph::OutputSlot out;
    CHECK(run_kernel(props, in, out) == ME_E_INVALID_ARG);
}

TEST_CASE("RenderAffineBlit: schema registered with correct ports") {
    register_once();
    const auto* info = task::lookup(task::TaskKindId::RenderAffineBlit);
    REQUIRE(info);
    REQUIRE(info->input_schema.size() == 1);
    CHECK(info->input_schema[0].type == graph::TypeId::RgbaFrame);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->output_schema[0].type == graph::TypeId::RgbaFrame);
    CHECK(info->time_invariant == true);
    CHECK(info->cacheable == true);
}
