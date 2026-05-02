/*
 * test_face_sticker_kernel_pixel — byte-identical pixel
 * regression for `me::compose::apply_face_sticker_inplace`
 * (M11 `pixel-regression-test-face-sticker-kernel`).
 *
 * Existing `tests/test_face_sticker_stub.cpp` covers argument-
 * shape rejection + per-pixel sentinel checks; this suite tightens
 * to FULL-FRAME byte-identical comparison against a hand-computed
 * expected buffer. The point: catch any unintentional drift in
 * the blend formula / affine sampling / bbox clipping that would
 * shift output pixels.
 *
 * Determinism: kernel uses fixed-point Porter-Duff source-over
 * with /255 round-half-up
 * (`(src * sa + dst * (255 - sa) + 127) / 255` per
 * `face_sticker_kernel.cpp:42`); affine_blit at scale 1.0 with
 * matching sticker / bbox dimensions degenerates to a 1:1 copy
 * (no inter-pixel interpolation). Both ops are integer-deterministic.
 *
 * Test fixture:
 *   - 16x16 black canvas, RGBA8 with alpha=255 everywhere.
 *   - 8x8 sticker, solid red @ 50% alpha (RGBA = 0xFF, 0x00, 0x00, 0x80).
 *   - One bbox at (4, 4)-(12, 12) — exactly 8x8, centered in the canvas.
 *   - params: scale 1.0, offset 0 → sticker fills the bbox identically.
 *
 * Expected pixel inside bbox (sa=128, dst=(0,0,0,255)):
 *   result.r = (255*128 + 0*127 + 127) / 255 = 32767 / 255 = 128
 *   result.g = (0  *128 + 0*127 + 127) / 255 =   127 / 255 =   0
 *   result.b = 0
 *   result.a = 128 + (255 * 127 + 127) / 255 = 128 + 32512/255
 *            = 128 + 127 = 255
 * Outside bbox: untouched (0, 0, 0, 255).
 */
#include <doctest/doctest.h>

#include "compose/bbox.hpp"
#include "compose/face_sticker_kernel.hpp"
#include "timeline/effect_params/face_sticker.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

constexpr int kCanvasW = 16;
constexpr int kCanvasH = 16;
constexpr int kStickerW = 8;
constexpr int kStickerH = 8;
constexpr int kBboxX0 = 4;
constexpr int kBboxY0 = 4;
constexpr int kBboxX1 = 12;
constexpr int kBboxY1 = 12;

std::vector<std::uint8_t> make_black_canvas() {
    std::vector<std::uint8_t> buf(kCanvasW * kCanvasH * 4, 0);
    /* Alpha = 255 everywhere so the blend has something to mix into. */
    for (std::size_t i = 3; i < buf.size(); i += 4) buf[i] = 0xFF;
    return buf;
}

std::vector<std::uint8_t> make_red_half_alpha_sticker() {
    std::vector<std::uint8_t> buf(kStickerW * kStickerH * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = 0xFF;  /* R */
        buf[i + 1] = 0x00;  /* G */
        buf[i + 2] = 0x00;  /* B */
        buf[i + 3] = 0x80;  /* A = 128 */
    }
    return buf;
}

/* Hand-computed expected output: 8x8 inside bbox = (128,0,0,255);
 * the rest = (0,0,0,255). Generates the same buffer apply_*
 * should produce. */
std::vector<std::uint8_t> make_expected() {
    auto buf = make_black_canvas();
    for (int y = kBboxY0; y < kBboxY1; ++y) {
        for (int x = kBboxX0; x < kBboxX1; ++x) {
            const std::size_t idx = (y * kCanvasW + x) * 4;
            buf[idx + 0] = 128;  /* R = (255*128 + 0*127 + 127)/255 */
            buf[idx + 1] = 0;
            buf[idx + 2] = 0;
            buf[idx + 3] = 255;  /* alpha composes to 255 */
        }
    }
    return buf;
}

}  // namespace

TEST_CASE("face_sticker pixel regression: byte-identical 16x16 frame after blend") {
    auto canvas   = make_black_canvas();
    auto sticker  = make_red_half_alpha_sticker();
    auto expected = make_expected();

    me::FaceStickerEffectParams p;
    p.landmark.asset_id = "test";
    p.sticker_uri       = "test://red-50.png";
    p.scale_x           = 1.0;
    p.scale_y           = 1.0;
    p.offset_x          = 0.0;
    p.offset_y          = 0.0;

    std::array<me::compose::Bbox, 1> bboxes = {{
        { kBboxX0, kBboxY0, kBboxX1, kBboxY1 }
    }};

    REQUIRE(me::compose::apply_face_sticker_inplace(
                canvas.data(), kCanvasW, kCanvasH,
                static_cast<std::size_t>(kCanvasW) * 4,
                p,
                std::span<const me::compose::Bbox>(bboxes.data(), bboxes.size()),
                sticker.data(), kStickerW, kStickerH,
                static_cast<std::size_t>(kStickerW) * 4) == ME_OK);

    /* Byte-identical compare. Any drift in the blend formula /
     * affine sampling / bbox clipping shows here as a per-pixel
     * mismatch; doctest reports the first failing index. */
    REQUIRE(canvas.size() == expected.size());
    for (std::size_t i = 0; i < canvas.size(); ++i) {
        if (canvas[i] != expected[i]) {
            const std::size_t pixel_idx = i / 4;
            const int x = static_cast<int>(pixel_idx % kCanvasW);
            const int y = static_cast<int>(pixel_idx / kCanvasW);
            const int channel = static_cast<int>(i % 4);
            FAIL("pixel mismatch at (x=", x, ", y=", y, ") channel=", channel,
                 " — got 0x", std::hex, static_cast<int>(canvas[i]),
                 ", expected 0x", static_cast<int>(expected[i]));
        }
    }
}

TEST_CASE("face_sticker pixel regression: opaque sticker overwrites bbox") {
    /* Variant: alpha=255 on the sticker → blend short-circuits to
     * memcpy. Verifies the opaque-fast-path branch in
     * blend_over_pixel (face_sticker_kernel.cpp:36-39) matches the
     * general-case formula at sa=255. */
    auto canvas  = make_black_canvas();
    std::vector<std::uint8_t> sticker(kStickerW * kStickerH * 4);
    for (std::size_t i = 0; i < sticker.size(); i += 4) {
        sticker[i + 0] = 0x40;  /* R = 64 */
        sticker[i + 1] = 0x80;  /* G = 128 */
        sticker[i + 2] = 0xC0;  /* B = 192 */
        sticker[i + 3] = 0xFF;  /* opaque */
    }

    me::FaceStickerEffectParams p;
    p.scale_x  = 1.0;
    p.scale_y  = 1.0;
    p.offset_x = 0.0;
    p.offset_y = 0.0;

    std::array<me::compose::Bbox, 1> bboxes = {{
        { kBboxX0, kBboxY0, kBboxX1, kBboxY1 }
    }};

    REQUIRE(me::compose::apply_face_sticker_inplace(
                canvas.data(), kCanvasW, kCanvasH,
                static_cast<std::size_t>(kCanvasW) * 4,
                p,
                std::span<const me::compose::Bbox>(bboxes.data(), bboxes.size()),
                sticker.data(), kStickerW, kStickerH,
                static_cast<std::size_t>(kStickerW) * 4) == ME_OK);

    /* Inside bbox: exact sticker bytes. Outside: untouched black. */
    for (int y = 0; y < kCanvasH; ++y) {
        for (int x = 0; x < kCanvasW; ++x) {
            const std::size_t idx = (y * kCanvasW + x) * 4;
            const bool inside = (x >= kBboxX0 && x < kBboxX1 &&
                                  y >= kBboxY0 && y < kBboxY1);
            if (inside) {
                CHECK(canvas[idx + 0] == 0x40);
                CHECK(canvas[idx + 1] == 0x80);
                CHECK(canvas[idx + 2] == 0xC0);
                CHECK(canvas[idx + 3] == 0xFF);
            } else {
                CHECK(canvas[idx + 0] == 0x00);
                CHECK(canvas[idx + 1] == 0x00);
                CHECK(canvas[idx + 2] == 0x00);
                CHECK(canvas[idx + 3] == 0xFF);
            }
        }
    }
}

TEST_CASE("face_sticker pixel regression: transparent sticker is no-op") {
    /* Variant: alpha=0 on the sticker → blend short-circuits with
     * "fully transparent — no op" (face_sticker_kernel.cpp:35).
     * Asserts the canvas is byte-identical before/after. */
    auto canvas_before = make_black_canvas();
    auto canvas        = make_black_canvas();
    std::vector<std::uint8_t> sticker(kStickerW * kStickerH * 4);
    for (std::size_t i = 0; i < sticker.size(); i += 4) {
        sticker[i + 0] = 0xFF;  /* whatever — alpha is 0 so RGB ignored */
        sticker[i + 1] = 0xFF;
        sticker[i + 2] = 0xFF;
        sticker[i + 3] = 0x00;  /* fully transparent */
    }

    me::FaceStickerEffectParams p;
    p.scale_x = 1.0;
    p.scale_y = 1.0;

    std::array<me::compose::Bbox, 1> bboxes = {{
        { kBboxX0, kBboxY0, kBboxX1, kBboxY1 }
    }};

    REQUIRE(me::compose::apply_face_sticker_inplace(
                canvas.data(), kCanvasW, kCanvasH,
                static_cast<std::size_t>(kCanvasW) * 4,
                p,
                std::span<const me::compose::Bbox>(bboxes.data(), bboxes.size()),
                sticker.data(), kStickerW, kStickerH,
                static_cast<std::size_t>(kStickerW) * 4) == ME_OK);
    CHECK(canvas == canvas_before);
}
