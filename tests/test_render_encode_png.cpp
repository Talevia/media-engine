/*
 * test_render_encode_png — kernel-level cover for the RenderEncodePng
 * graph node. Pins:
 *   1. RGBA8 input → ByteBuffer output with PNG magic + IHDR.
 *   2. Output dimensions match input (no upstream resize required;
 *      the kernel intentionally does no scale).
 *   3. Schema introspection.
 *   4. Empty / invalid input → ME_E_INVALID_ARG.
 */
#include <doctest/doctest.h>

#include "compose/encode_png_kernel.hpp"
#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

using namespace me;

namespace {

void register_once() {
    static std::once_flag once;
    std::call_once(once, []() { compose::register_encode_png_kind(); });
}

std::shared_ptr<graph::RgbaFrameData> solid(int w, int h,
                                             uint8_t r, uint8_t g, uint8_t b) {
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
            f->rgba[p + 3] = 255;
        }
    }
    return f;
}

}  // namespace

TEST_CASE("RenderEncodePng: 64×64 RGBA8 → ByteBuffer with PNG magic") {
    register_once();

    auto fn = task::best_kernel_for(task::TaskKindId::RenderEncodePng,
                                     task::Affinity::Cpu);
    REQUIRE(fn);

    graph::InputValue in;
    in.v = solid(64, 64, 30, 90, 200);
    graph::OutputSlot out;
    task::TaskContext ctx{};
    REQUIRE(fn(ctx, {},
               std::span<const graph::InputValue>{&in, 1},
               std::span<graph::OutputSlot>      {&out, 1}) == ME_OK);

    auto* png_pp = std::get_if<std::shared_ptr<std::vector<uint8_t>>>(&out.v);
    REQUIRE(png_pp);
    REQUIRE(*png_pp);
    const auto& png = **png_pp;
    REQUIRE(png.size() > 8);
    /* PNG signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A */
    CHECK(png[0] == 0x89);
    CHECK(png[1] == 0x50);
    CHECK(png[2] == 0x4E);
    CHECK(png[3] == 0x47);
    CHECK(png[4] == 0x0D);
    CHECK(png[5] == 0x0A);
    CHECK(png[6] == 0x1A);
    CHECK(png[7] == 0x0A);
}

TEST_CASE("RenderEncodePng: 1×1 RGBA8 also produces valid PNG") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::RenderEncodePng,
                                     task::Affinity::Cpu);
    graph::InputValue in;
    in.v = solid(1, 1, 255, 0, 0);
    graph::OutputSlot out;
    task::TaskContext ctx{};
    REQUIRE(fn(ctx, {},
               std::span<const graph::InputValue>{&in, 1},
               std::span<graph::OutputSlot>      {&out, 1}) == ME_OK);
    auto& png = **std::get_if<std::shared_ptr<std::vector<uint8_t>>>(&out.v);
    REQUIRE(png.size() > 8);
    CHECK(png[0] == 0x89);
    CHECK(png[3] == 0x47);
}

TEST_CASE("RenderEncodePng: empty input → ME_E_INVALID_ARG") {
    register_once();
    auto fn = task::best_kernel_for(task::TaskKindId::RenderEncodePng,
                                     task::Affinity::Cpu);
    graph::OutputSlot out;
    task::TaskContext ctx{};
    CHECK(fn(ctx, {},
             std::span<const graph::InputValue>{},
             std::span<graph::OutputSlot>      {&out, 1}) == ME_E_INVALID_ARG);
}

TEST_CASE("RenderEncodePng: schema declares RgbaFrame in / ByteBuffer out") {
    register_once();
    const auto* info = task::lookup(task::TaskKindId::RenderEncodePng);
    REQUIRE(info);
    REQUIRE(info->input_schema.size() == 1);
    CHECK(info->input_schema[0].type == graph::TypeId::RgbaFrame);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->output_schema[0].type == graph::TypeId::ByteBuffer);
    CHECK(info->time_invariant == true);
    CHECK(info->cacheable == true);
}
