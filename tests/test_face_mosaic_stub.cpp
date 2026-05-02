/*
 * test_face_mosaic_stub — coverage for
 * `me::compose::apply_face_mosaic_inplace` post-`face-mosaic-impl`.
 * Pre-cycle the kernel was a typed-reject stub returning
 * ME_E_UNSUPPORTED; this suite now covers the real Pixelate +
 * Blur math given pre-resolved bboxes.
 *
 * Suite name kept ending in `_stub` for ctest target stability.
 */
#include <doctest/doctest.h>

#include "compose/bbox.hpp"
#include "compose/face_mosaic_kernel.hpp"
#include "timeline/timeline_impl.hpp"
#include "timeline_schema_fixtures.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

using me::tests::schema::EngineFixture;
using me::tests::schema::load;

namespace {

/* RGB checkerboard: alternating pixels are (255, 0, 0) red and
 * (0, 0, 255) blue, alpha=255. Useful because mean-over-tile
 * collapses to (≈128, 0, ≈128) — visibly different from the
 * input pattern. */
std::vector<std::uint8_t> make_checkerboard(int w, int h) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const bool red = ((x + y) & 1) == 0;
            buf[i + 0] = red ? 255 : 0;
            buf[i + 1] = 0;
            buf[i + 2] = red ? 0 : 255;
            buf[i + 3] = 255;
        }
    }
    return buf;
}

me::FaceMosaicEffectParams pixelate_params(int block_size = 4) {
    me::FaceMosaicEffectParams p;
    p.landmark.asset_id = "ml1";
    p.block_size_px     = block_size;
    p.kind              = me::FaceMosaicEffectParams::Kind::Pixelate;
    return p;
}

me::FaceMosaicEffectParams blur_params(int block_size = 4) {
    me::FaceMosaicEffectParams p;
    p.landmark.asset_id = "ml1";
    p.block_size_px     = block_size;
    p.kind              = me::FaceMosaicEffectParams::Kind::Blur;
    return p;
}

}  // namespace

TEST_CASE("face_mosaic: null buffer rejected") {
    auto p = pixelate_params();
    CHECK(me::compose::apply_face_mosaic_inplace(
        nullptr, 16, 16, 64, p, {}) == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic: non-positive dimensions rejected") {
    auto buf = make_checkerboard(16, 16);
    auto p = pixelate_params();
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 0, 16, 64, p, {}) == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 16, -1, 64, p, {}) == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic: undersized stride rejected") {
    auto buf = make_checkerboard(16, 16);
    auto p = pixelate_params();
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 16, 16, 32, p, {}) == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic: non-positive block_size_px rejected") {
    auto buf = make_checkerboard(16, 16);
    auto p = pixelate_params();
    p.block_size_px = 0;
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 16, 16, 64, p, {}) == ME_E_INVALID_ARG);
    p.block_size_px = -8;
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 16, 16, 64, p, {}) == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic: empty bboxes → no-op (frame unchanged)") {
    auto buf = make_checkerboard(8, 8);
    const auto snapshot = buf;
    auto p = pixelate_params();
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p, {}) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_mosaic: zero-area bbox is a no-op") {
    auto buf = make_checkerboard(8, 8);
    const auto snapshot = buf;
    auto p = pixelate_params();
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{4, 4, 4, 4}};
    CHECK(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p, std::span<const me::compose::Bbox>(bboxes))
        == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_mosaic Pixelate: 4×4 tile means red+blue checker → midpoint gray-purple") {
    /* Checkerboard inside an 8×8 frame; a 4×4 tile covers exactly
     * 8 red + 8 blue pixels. Mean R = 8*255/16 = 127.5 → 128 with
     * round-half-up. Mean G = 0. Mean B = 8*255/16 = 128. So every
     * pixel inside the tile becomes (128, 0, 128). */
    auto buf = make_checkerboard(8, 8);
    auto p = pixelate_params(4);

    /* Bbox covers the whole frame. */
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 8, 8}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);

    /* Every pixel should be (128, 0, 128, 255). */
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const std::size_t i = (y * 8 + x) * 4;
            CHECK(buf[i + 0] == 128);
            CHECK(buf[i + 1] == 0);
            CHECK(buf[i + 2] == 128);
            CHECK(buf[i + 3] == 255);  /* alpha preserved */
        }
    }
}

TEST_CASE("face_mosaic Pixelate: bbox restricts effect; outside untouched") {
    auto buf = make_checkerboard(8, 8);
    auto p = pixelate_params(4);

    /* Apply only inside (0,0)-(4,4); outside should keep checkerboard. */
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 4, 4}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);

    /* Inside (0..4, 0..4): mosaic'd to ~(128, 0, 128). */
    const std::size_t inside = 0;
    CHECK(buf[inside + 0] == 128);
    CHECK(buf[inside + 1] == 0);
    CHECK(buf[inside + 2] == 128);

    /* Outside (5, 5) — original checker. (5 + 5) is even → red. */
    const std::size_t outside_red = (5 * 8 + 5) * 4;
    CHECK(buf[outside_red + 0] == 255);
    CHECK(buf[outside_red + 1] == 0);
    CHECK(buf[outside_red + 2] == 0);
}

TEST_CASE("face_mosaic Pixelate: alpha channel preserved per-pixel") {
    /* Build a frame where alpha varies per pixel — the mosaic
     * should pixelate RGB but leave per-pixel alpha alone. */
    std::vector<std::uint8_t> buf(8 * 8 * 4);
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const std::size_t i = (y * 8 + x) * 4;
            buf[i + 0] = 100;
            buf[i + 1] = 100;
            buf[i + 2] = 100;
            buf[i + 3] = static_cast<std::uint8_t>(x * 32);  /* gradient */
        }
    }

    auto p = pixelate_params(4);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 8, 8}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);

    /* Alpha at (0, 0) was 0; at (3, 0) was 96. Both should be unchanged. */
    CHECK(buf[(0 * 8 + 0) * 4 + 3] == 0);
    CHECK(buf[(0 * 8 + 3) * 4 + 3] == 96);
}

TEST_CASE("face_mosaic Blur: uniform region stays uniform") {
    /* Blur over a uniform-color region produces the same color
     * (no edges to smooth). */
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(8) * 8 * 4, 0);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = 200; buf[i + 1] = 100; buf[i + 2] = 50; buf[i + 3] = 255;
    }
    const auto snapshot = buf;
    auto p = blur_params(4);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 8, 8}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_mosaic Blur: edge between black/white softens") {
    /* Half black, half white — blur should produce intermediate
     * values along the seam. */
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(8) * 8 * 4, 0);
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const std::size_t i = (y * 8 + x) * 4;
            const std::uint8_t v = (x < 4) ? 0 : 255;
            buf[i + 0] = v; buf[i + 1] = v; buf[i + 2] = v; buf[i + 3] = 255;
        }
    }
    auto p = blur_params(4);  /* radius = 2 */
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 8, 8}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);

    /* Pixel at (3, 4) was originally 0 (black). With radius=2, the
     * blur kernel at x=3 averages x in [1,5], pulling some white
     * in. Expect a non-zero intermediate value. */
    const std::size_t i_left  = (4 * 8 + 3) * 4;
    CHECK(buf[i_left + 0] > 0);
    CHECK(buf[i_left + 0] < 255);

    /* Far-from-seam pixel (0, 0) was black. With radius=2, blur
     * still sees only black columns (0..2) → stays 0. */
    const std::size_t i_far = (0 * 8 + 0) * 4;
    CHECK(buf[i_far + 0] == 0);
}

TEST_CASE("face_mosaic: bbox extending past image bounds is clamped") {
    auto buf = make_checkerboard(8, 8);
    auto p = pixelate_params(4);

    /* bbox extends past the right/bottom edges; kernel should
     * clamp + still produce no crash. */
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{4, 4, 24, 24}};

    REQUIRE(me::compose::apply_face_mosaic_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes)) == ME_OK);

    /* The clipped 4×4 region (4..8, 4..8) should be mosaic'd. */
    const std::size_t i = (4 * 8 + 4) * 4;
    CHECK(buf[i + 0] == 128);
    CHECK(buf[i + 1] == 0);
    CHECK(buf[i + 2] == 128);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string face_mosaic_timeline(const std::string& effect_body) {
    return std::string{R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/landmarks.bin","type":"landmark",
     "model":{"id":"blazeface","version":"v2","quantization":"fp16"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "effects":[)"} + effect_body + R"(],
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
}

}  // namespace

TEST_CASE("face_mosaic JSON: full param set parses into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{
            "landmarkAssetId":"ml1",
            "blockSizePx":24,
            "kind":"blur"
        }
    })";
    REQUIRE(load(f.eng, face_mosaic_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& effects = tl->tl.clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::FaceMosaic);
    const auto* fp = std::get_if<me::FaceMosaicEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "ml1");
    CHECK(fp->block_size_px == 24);
    CHECK(fp->kind == me::FaceMosaicEffectParams::Kind::Blur);
    me_timeline_destroy(tl);
}

TEST_CASE("face_mosaic JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1"}
    })";
    REQUIRE(load(f.eng, face_mosaic_timeline(body), &tl) == ME_OK);
    const auto& fp = std::get<me::FaceMosaicEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(fp.block_size_px == 16);
    CHECK(fp.kind == me::FaceMosaicEffectParams::Kind::Pixelate);
    me_timeline_destroy(tl);
}

TEST_CASE("face_mosaic JSON: missing landmarkAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"blockSizePx":16}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("face_mosaic JSON: blockSizePx out of range → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body_zero = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","blockSizePx":0}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body_zero), &tl) == ME_E_PARSE);
    const std::string body_huge = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","blockSizePx":99999}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body_huge), &tl) == ME_E_PARSE);
}

TEST_CASE("face_mosaic JSON: unknown kind value → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","kind":"swirl"}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body), &tl) == ME_E_PARSE);
}
