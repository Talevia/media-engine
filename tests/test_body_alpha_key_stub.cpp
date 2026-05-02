/*
 * test_body_alpha_key_stub — coverage for
 * `me::compose::apply_body_alpha_key_inplace` post-`body-alpha-key-impl`.
 * Pre-cycle the kernel was a typed-reject stub returning
 * ME_E_UNSUPPORTED; this suite now covers the real
 * mask-driven alpha key math.
 *
 * Suite name kept ending in `_stub` for ctest target stability.
 */
#include <doctest/doctest.h>

#include "compose/body_alpha_key_kernel.hpp"
#include "timeline/timeline_impl.hpp"
#include "timeline_schema_fixtures.hpp"

#include <cstdint>
#include <variant>
#include <vector>

using me::tests::schema::EngineFixture;
using me::tests::schema::load;

namespace {

/* RGBA buffer with all pixels having the same color + alpha. */
std::vector<std::uint8_t> make_solid(int w, int h,
                                       std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = a;
    }
    return buf;
}

/* Single-channel mask buffer with a specified per-pixel value. */
std::vector<std::uint8_t> make_mask(int w, int h, std::uint8_t v) {
    return std::vector<std::uint8_t>(
        static_cast<std::size_t>(w) * h, v);
}

me::BodyAlphaKeyEffectParams plain_params() {
    me::BodyAlphaKeyEffectParams p;
    p.mask.asset_id     = "mask1";
    p.feather_radius_px = 0;
    p.invert            = false;
    return p;
}

}  // namespace

TEST_CASE("body_alpha_key: null buffer rejected") {
    auto p = plain_params();
    auto mask = make_mask(16, 16, 255);
    CHECK(me::compose::apply_body_alpha_key_inplace(
        nullptr, 16, 16, 64, p, mask.data(), 16, 16, 16) == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: non-positive dimensions rejected") {
    auto buf = make_solid(16, 16, 200, 100, 50, 255);
    auto mask = make_mask(16, 16, 255);
    auto p = plain_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 0, 16, 64, p, mask.data(), 16, 16, 16) == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 16, -1, 64, p, mask.data(), 16, 16, 16) == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: undersized stride rejected") {
    auto buf = make_solid(16, 16, 200, 100, 50, 255);
    auto mask = make_mask(16, 16, 255);
    auto p = plain_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 16, 16, 32, p, mask.data(), 16, 16, 16) == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: negative feather_radius_px rejected") {
    auto buf = make_solid(16, 16, 200, 100, 50, 255);
    auto mask = make_mask(16, 16, 255);
    auto p = plain_params();
    p.feather_radius_px = -1;
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 16, 16, 64, p, mask.data(), 16, 16, 16) == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: null mask → no-op") {
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    const auto snapshot = buf;
    auto p = plain_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, nullptr, 0, 0, 0) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("body_alpha_key: mask dims must match frame dims") {
    auto buf = make_solid(16, 16, 200, 100, 50, 255);
    auto mask_small = make_mask(8, 8, 255);
    auto p = plain_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 16, 16, 64, p, mask_small.data(), 8, 8, 8)
        == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: undersized mask_stride rejected") {
    auto buf = make_solid(16, 16, 200, 100, 50, 255);
    auto mask = make_mask(16, 16, 255);
    auto p = plain_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 16, 16, 64, p, mask.data(), 16, 16, 8)
        == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key: opaque mask preserves input alpha (255 * 255 / 255 = 255)") {
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    auto mask = make_mask(8, 8, 255);
    auto p = plain_params();
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);
    /* Every pixel: RGB unchanged, alpha = 255. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == 200);
        CHECK(buf[i + 1] == 100);
        CHECK(buf[i + 2] == 50);
        CHECK(buf[i + 3] == 255);
    }
}

TEST_CASE("body_alpha_key: zero mask zeroes alpha (255 * 0 / 255 = 0)") {
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    auto mask = make_mask(8, 8, 0);
    auto p = plain_params();
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);
    /* RGB unchanged, alpha = 0. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == 200);
        CHECK(buf[i + 1] == 100);
        CHECK(buf[i + 2] == 50);
        CHECK(buf[i + 3] == 0);
    }
}

TEST_CASE("body_alpha_key: 50% mask halves alpha (255 * 128 / 255 ≈ 128)") {
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    auto mask = make_mask(8, 8, 128);
    auto p = plain_params();
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);
    /* alpha = (255 * 128 + 127) / 255 = 128. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == 200);
        CHECK(buf[i + 3] == 128);
    }
}

TEST_CASE("body_alpha_key: invert flips mask semantics") {
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    auto mask = make_mask(8, 8, 0);  /* "all background" mask */
    auto p = plain_params();
    p.invert = true;                  /* invert: 0 → 255, so foreground */
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);
    /* With invert=true and mask=0, effective=255, alpha stays 255. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 3] == 255);
    }
}

TEST_CASE("body_alpha_key: feather softens mask edge") {
    /* Build an 8×8 mask: left half 0 (background), right half 255
     * (foreground). With feather radius 2, the edge between cols 3
     * and 4 should produce intermediate alpha values nearby. */
    std::vector<std::uint8_t> mask(8 * 8, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            mask[static_cast<std::size_t>(y) * 8 + x] = 255;
        }
    }
    auto buf = make_solid(8, 8, 200, 100, 50, 255);
    auto p = plain_params();
    p.feather_radius_px = 2;
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);

    /* Pixel at (3, 4) is on the background side adjacent to the
     * edge — feathered alpha should be > 0 (some foreground bled
     * in) but < 255. */
    const std::size_t i_edge = (4 * 8 + 3) * 4;
    CHECK(buf[i_edge + 3] > 0);
    CHECK(buf[i_edge + 3] < 255);

    /* Pixel at (0, 0) is far from the edge in a 2-radius feather:
     * with radius=2, blur sees columns 0..2 (all 0) → still 0. */
    const std::size_t i_far = 0;
    CHECK(buf[i_far + 3] == 0);

    /* Pixel at (7, 7) is far from the edge in foreground: blur
     * sees columns 5..7 (all 255) → still 255. */
    const std::size_t i_solid = (7 * 8 + 7) * 4;
    CHECK(buf[i_solid + 3] == 255);
}

TEST_CASE("body_alpha_key: RGB channels never modified") {
    /* Build an RGBA buffer with a per-pixel color gradient; verify
     * RGB is byte-identical after applying the alpha key. */
    std::vector<std::uint8_t> buf(8 * 8 * 4);
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const std::size_t i = (y * 8 + x) * 4;
            buf[i + 0] = static_cast<std::uint8_t>(x * 32);
            buf[i + 1] = static_cast<std::uint8_t>(y * 32);
            buf[i + 2] = static_cast<std::uint8_t>((x + y) * 16);
            buf[i + 3] = 255;
        }
    }
    /* Snapshot just the RGB triplets (skip alpha). */
    std::vector<std::uint8_t> rgb_before;
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        rgb_before.push_back(buf[i + 0]);
        rgb_before.push_back(buf[i + 1]);
        rgb_before.push_back(buf[i + 2]);
    }

    auto mask = make_mask(8, 8, 64);
    auto p = plain_params();
    REQUIRE(me::compose::apply_body_alpha_key_inplace(
        buf.data(), 8, 8, 32, p, mask.data(), 8, 8, 8) == ME_OK);

    std::vector<std::uint8_t> rgb_after;
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        rgb_after.push_back(buf[i + 0]);
        rgb_after.push_back(buf[i + 1]);
        rgb_after.push_back(buf[i + 2]);
    }
    CHECK(rgb_before == rgb_after);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string body_alpha_key_timeline(const std::string& effect_body) {
    return std::string{R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"mask1","uri":"file:///tmp/mask.bin","type":"mask",
     "model":{"id":"selfie_seg","version":"v3","quantization":"int8"}}
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

TEST_CASE("body_alpha_key JSON: full param set parses into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{
            "maskAssetId":"mask1",
            "featherRadiusPx":8,
            "invert":true
        }
    })";
    REQUIRE(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& effects = tl->tl.clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::BodyAlphaKey);
    const auto* bp = std::get_if<me::BodyAlphaKeyEffectParams>(&effects[0].params);
    REQUIRE(bp != nullptr);
    CHECK(bp->mask.asset_id == "mask1");
    CHECK(bp->feather_radius_px == 8);
    CHECK(bp->invert == true);
    me_timeline_destroy(tl);
}

TEST_CASE("body_alpha_key JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1"}
    })";
    REQUIRE(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_OK);
    const auto& bp = std::get<me::BodyAlphaKeyEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(bp.feather_radius_px == 0);
    CHECK(bp.invert == false);
    me_timeline_destroy(tl);
}

TEST_CASE("body_alpha_key JSON: missing maskAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"featherRadiusPx":4}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("body_alpha_key JSON: featherRadiusPx out of range → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body_neg = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","featherRadiusPx":-3}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body_neg), &tl) == ME_E_PARSE);
    const std::string body_huge = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","featherRadiusPx":9999}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body_huge), &tl) == ME_E_PARSE);
}

TEST_CASE("body_alpha_key JSON: invert non-boolean → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","invert":"true"}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_E_PARSE);
}
