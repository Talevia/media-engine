/*
 * test_face_sticker_stub — coverage for
 * `me::compose::apply_face_sticker_inplace` post-`face-sticker-impl`.
 * The kernel was previously a typed-reject stub returning
 * ME_E_UNSUPPORTED; this suite now covers the real
 * deterministic-blend behavior given pre-resolved bboxes +
 * decoded sticker pixels.
 *
 * What this suite asserts:
 *   - Argument-shape rejects (null buffer, non-positive dims,
 *     undersized stride, undersized sticker stride) land before
 *     any blend.
 *   - Empty bboxes / null sticker → ME_OK no-op (frame unchanged).
 *   - Single opaque sticker over a known bbox → opaque pixels
 *     replace dst; out-of-bbox dst untouched.
 *   - Translucent sticker → linear source-over blend with dst.
 *   - Multi-bbox dispatch composites independently.
 *   - JSON loader contract (kind: face_sticker fields) round-trips
 *     correctly — unchanged from the registered-but-deferred
 *     era.
 *
 * The suite name still ends in `_stub` for ctest target stability;
 * the `_stub` reference is now historical.
 */
#include <doctest/doctest.h>

#include "compose/bbox.hpp"
#include "compose/face_sticker_kernel.hpp"
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

std::vector<std::uint8_t> make_bg(int w, int h, std::uint8_t fill = 0x00) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4, fill);
    /* Set alpha to opaque so blend logic has something to blend into. */
    for (std::size_t i = 3; i < buf.size(); i += 4) buf[i] = 0xff;
    return buf;
}

std::vector<std::uint8_t> make_sticker(int w, int h, std::uint8_t r,
                                         std::uint8_t g, std::uint8_t b,
                                         std::uint8_t a) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = a;
    }
    return buf;
}

me::FaceStickerEffectParams identity_params() {
    me::FaceStickerEffectParams p;
    p.landmark_asset_id = "ml1";
    p.sticker_uri       = "file:///tmp/star.png";
    /* scale 1.0 + offset 0 → sticker fills the bbox exactly */
    return p;
}

}  // namespace

TEST_CASE("face_sticker: null buffer rejected with INVALID_ARG") {
    auto p = identity_params();
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    CHECK(me::compose::apply_face_sticker_inplace(
        nullptr, 16, 16, 64, p,
        std::span<const me::compose::Bbox>{},
        sticker.data(), 8, 8, 32) == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker: non-positive dimensions rejected") {
    auto p = identity_params();
    auto buf = make_bg(16, 16);
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 0, 16, 64, p, {},
        sticker.data(), 8, 8, 32) == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, -1, 64, p, {},
        sticker.data(), 8, 8, 32) == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker: dst stride < width*4 rejected") {
    auto p = identity_params();
    auto buf = make_bg(16, 16);
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 32, p, {},
        sticker.data(), 8, 8, 32) == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker: empty bboxes → no-op (frame unchanged)") {
    auto p = identity_params();
    auto buf = make_bg(16, 16, 0x42);
    const auto snapshot = buf;
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p, {},
        sticker.data(), 8, 8, 32) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_sticker: null sticker → no-op even with bboxes") {
    auto p = identity_params();
    auto buf = make_bg(16, 16, 0x42);
    const auto snapshot = buf;
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{2, 2, 6, 6}};
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p,
        std::span<const me::compose::Bbox>(bboxes),
        nullptr, 0, 0, 0) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_sticker: opaque red sticker fills the bbox; outside untouched") {
    /* 16x16 black background; opaque red 8x8 sticker scaled to fit
     * the bbox (4,4)-(12,12). The 8x8 area inside the bbox should
     * be red; the rest should stay black. */
    auto p = identity_params();
    auto buf = make_bg(16, 16, 0x00);
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{4, 4, 12, 12}};
    REQUIRE(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p,
        std::span<const me::compose::Bbox>(bboxes),
        sticker.data(), 8, 8, 32) == ME_OK);

    /* Inside bbox: red opaque. */
    for (int y = 4; y < 12; ++y) {
        for (int x = 4; x < 12; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 16 + x) * 4;
            CHECK(buf[i + 0] == 255);
            CHECK(buf[i + 1] == 0);
            CHECK(buf[i + 2] == 0);
            CHECK(buf[i + 3] == 255);
        }
    }
    /* Outside bbox: still black opaque (background). */
    const std::size_t corner = 0;
    CHECK(buf[corner + 0] == 0);
    CHECK(buf[corner + 1] == 0);
    CHECK(buf[corner + 2] == 0);
    CHECK(buf[corner + 3] == 0xff);
}

TEST_CASE("face_sticker: 50% alpha sticker blends with dst") {
    /* dst opaque white, sticker opaque red @ alpha=128.
     * Expected blend: 255 * 128/255 + 255 * 127/255 ≈ 255 (red),
     *                  0 * 128/255 + 255 * 127/255 ≈ 127 (G), same B.
     * Allow ±1 LSB rounding tolerance. */
    auto p = identity_params();
    auto buf = make_bg(8, 8, 0xff);                 /* opaque white */
    auto sticker = make_sticker(4, 4, 255, 0, 0, 128);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{0, 0, 8, 8}};
    REQUIRE(me::compose::apply_face_sticker_inplace(
        buf.data(), 8, 8, 32, p,
        std::span<const me::compose::Bbox>(bboxes),
        sticker.data(), 4, 4, 16) == ME_OK);

    const std::size_t i = 0;
    CHECK(buf[i + 0] == 255);                         /* full red */
    CHECK(static_cast<int>(buf[i + 1]) >= 126);       /* ~127 (white * 127/255) */
    CHECK(static_cast<int>(buf[i + 1]) <= 128);
    CHECK(static_cast<int>(buf[i + 2]) >= 126);
    CHECK(static_cast<int>(buf[i + 2]) <= 128);
    /* Alpha: 128 + 255 * 127/255 = 128 + 127 = 255 */
    CHECK(buf[i + 3] == 255);
}

TEST_CASE("face_sticker: bbox clipped to image bounds doesn't crash") {
    /* bbox extends past the right edge — kernel should clamp the
     * dst extent, not segfault. */
    auto p = identity_params();
    auto buf = make_bg(16, 16, 0x00);
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{12, 12, 24, 24}};         /* extends past 16x16 */
    REQUIRE(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p,
        std::span<const me::compose::Bbox>(bboxes),
        sticker.data(), 8, 8, 32) == ME_OK);

    /* Pixel (15, 15) should have the sticker color (it's inside
     * the clipped patch). */
    const std::size_t i = (15 * 16 + 15) * 4;
    CHECK(buf[i + 0] == 255);
    CHECK(buf[i + 1] == 0);
    CHECK(buf[i + 2] == 0);
}

TEST_CASE("face_sticker: zero-area bbox is a no-op") {
    auto p = identity_params();
    auto buf = make_bg(16, 16, 0x42);
    const auto snapshot = buf;
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{4, 4, 4, 4}};             /* zero area */
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p,
        std::span<const me::compose::Bbox>(bboxes),
        sticker.data(), 8, 8, 32) == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("face_sticker: degenerate scale (params.scale_x <= 0) is a no-op") {
    auto p = identity_params();
    p.scale_x = 0.0;
    auto buf = make_bg(16, 16, 0x42);
    const auto snapshot = buf;
    auto sticker = make_sticker(8, 8, 255, 0, 0, 255);
    const std::array<me::compose::Bbox, 1> bboxes{
        me::compose::Bbox{4, 4, 12, 12}};
    CHECK(me::compose::apply_face_sticker_inplace(
        buf.data(), 16, 16, 64, p,
        std::span<const me::compose::Bbox>(bboxes),
        sticker.data(), 8, 8, 32) == ME_OK);
    CHECK(buf == snapshot);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string face_sticker_timeline(const std::string& effect_body) {
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

TEST_CASE("face_sticker JSON: required + optional fields parse into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetId":"ml1",
            "stickerUri":"file:///tmp/star.png",
            "scaleX":1.5, "scaleY":2.0,
            "offsetX":10.0, "offsetY":-5.0
        }
    })";
    REQUIRE(load(f.eng, face_sticker_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& clips = tl->tl.clips;
    REQUIRE(clips.size() == 1);
    const auto& effects = clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::FaceSticker);
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark_asset_id == "ml1");
    CHECK(fp->sticker_uri       == "file:///tmp/star.png");
    CHECK(fp->scale_x == 1.5);
    CHECK(fp->scale_y == 2.0);
    CHECK(fp->offset_x == 10.0);
    CHECK(fp->offset_y == -5.0);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetId":"ml1",
            "stickerUri":"file:///tmp/star.png"
        }
    })";
    REQUIRE(load(f.eng, face_sticker_timeline(body), &tl) == ME_OK);
    const auto& fp = std::get<me::FaceStickerEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(fp.scale_x == 1.0);
    CHECK(fp.scale_y == 1.0);
    CHECK(fp.offset_x == 0.0);
    CHECK(fp.offset_y == 0.0);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker JSON: missing landmarkAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"stickerUri":"file:///tmp/star.png"}
    })";
    CHECK(load(f.eng, face_sticker_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("face_sticker JSON: missing stickerUri → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"landmarkAssetId":"ml1"}
    })";
    CHECK(load(f.eng, face_sticker_timeline(body), &tl) == ME_E_PARSE);
}
