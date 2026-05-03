/*
 * test_face_sticker_compose_stage — end-to-end coverage for the
 * face_sticker compose-stage upstream resolvers (M11
 * `face-sticker-compose-stage-wiring`):
 *
 *   1. Encode a tiny PNG sticker at runtime via libavcodec's PNG
 *      encoder (no fixture binary checked into the repo).
 *   2. Write a synthetic landmark JSON fixture next to the PNG.
 *   3. Call `me::compose::decode_sticker_to_rgba8` on the PNG;
 *      assert dimensions + a sentinel pixel.
 *   4. Call `me::compose::resolve_landmark_bboxes_from_file` for
 *      a couple of frame times; assert the resolver picks the
 *      closest frame's bboxes.
 *   5. Call `me::compose::apply_face_sticker_inplace` with the
 *      decoded sticker + resolved bboxes against a 64x64 RGBA8
 *      black canvas; assert the canvas now has the sticker's
 *      sentinel color inside the bbox + black outside.
 *
 * The PNG is encoded fresh per test run so there's no checked-in
 * binary fixture; the test cleans up its tempfiles in a guard
 * dtor. Same shape applies to face_mosaic + body_alpha_key — the
 * landmark resolver here is shared with face_mosaic, and a
 * sibling `body_alpha_key`-shaped test would use a (yet-to-be-
 * implemented) mask_resolver.
 */
#include <doctest/doctest.h>

#include "compose/bbox.hpp"
#include "compose/face_sticker_kernel.hpp"
#include "compose/landmark_resolver.hpp"
#include "compose/sticker_decoder.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

/* RAII scrub for tempfiles so re-runs don't leave bytes lying
 * around. The fixtures live in /tmp/me_test_face_sticker_*; the
 * dtor unlinks them on test exit. */
struct TempScrub {
    std::vector<std::string> paths;
    ~TempScrub() { for (const auto& p : paths) std::remove(p.c_str()); }
};

/* Encode a `width x height` solid-color RGBA8 image as PNG bytes
 * via libavcodec. Color is the same `{R,G,B,A}` for every pixel.
 * The PNG bytes are written to `out_path`. Returns true on
 * success. */
bool encode_solid_png(const std::string& out_path,
                       int width, int height,
                       std::uint8_t r, std::uint8_t g,
                       std::uint8_t b, std::uint8_t a) {
    /* Build RGBA buffer. */
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
    /* swscale RGBA → RGBA (PNG encoder accepts AV_PIX_FMT_RGBA
     * directly, so the conversion is identity-shaped — but
     * filling an AVFrame still requires going through
     * av_frame_get_buffer to align rows). */
    me::io::AvFramePtr frame(av_frame_alloc());
    if (!frame) return false;
    frame->format = AV_PIX_FMT_RGBA;
    frame->width  = width;
    frame->height = height;
    if (av_frame_get_buffer(frame.get(), 32) < 0) return false;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* dst = frame->data[0] + y * frame->linesize[0];
        const std::uint8_t* src = rgba.data() + y * width * 4;
        std::memcpy(dst, src, static_cast<std::size_t>(width) * 4);
    }

    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!enc) return false;
    me::io::AvCodecContextPtr ctx(avcodec_alloc_context3(enc));
    if (!ctx) return false;
    ctx->width     = width;
    ctx->height    = height;
    ctx->pix_fmt   = AV_PIX_FMT_RGBA;
    ctx->time_base = AVRational{1, 25};
    if (avcodec_open2(ctx.get(), enc, nullptr) < 0) return false;
    if (avcodec_send_frame(ctx.get(), frame.get()) < 0) return false;
    if (avcodec_send_frame(ctx.get(), nullptr) < 0) {
        /* Some encoders need flush; ignore failure. */
    }
    me::io::AvPacketPtr pkt(av_packet_alloc());
    if (!pkt) return false;
    if (avcodec_receive_packet(ctx.get(), pkt.get()) < 0) return false;
    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
    return out.good();
}

}  // namespace

TEST_CASE("decode_sticker_to_rgba8: round-trips a solid red 8x8 PNG") {
    TempScrub guard;
    const std::string png_path = "/tmp/me_test_face_sticker_red.png";
    guard.paths.push_back(png_path);
    REQUIRE(encode_solid_png(png_path, 8, 8, 0xFF, 0x00, 0x00, 0xFF));

    me::compose::StickerImage out;
    std::string err;
    REQUIRE_MESSAGE(
        me::compose::decode_sticker_to_rgba8("file://" + png_path, &out, &err)
            == ME_OK,
        err);
    CHECK(out.width  == 8);
    CHECK(out.height == 8);
    REQUIRE(out.pixels.size() == 8u * 8u * 4u);
    /* Pixel (0,0) — bytes 0..3 — must be the red we encoded. */
    CHECK(out.pixels[0] == 0xFF);
    CHECK(out.pixels[1] == 0x00);
    CHECK(out.pixels[2] == 0x00);
    CHECK(out.pixels[3] == 0xFF);
}

TEST_CASE("decode_sticker_to_rgba8: empty URI rejected") {
    me::compose::StickerImage out;
    std::string err;
    CHECK(me::compose::decode_sticker_to_rgba8("", &out, &err) == ME_E_INVALID_ARG);
    CHECK(err.find("uri is empty") != std::string::npos);
}

TEST_CASE("decode_sticker_to_rgba8: unsupported scheme rejected") {
    /* http(s):// passes through to libavformat's HTTP handler
     * (LGPL-clean, supported as of the m13-http-source-bootstrap
     * cycle). Schemes like asset:// / s3:// / custom resolvers
     * remain rejected with the named diag. */
    me::compose::StickerImage out;
    std::string err;
    CHECK(me::compose::decode_sticker_to_rgba8("asset:///pkg/sticker.png",
                                                  &out, &err)
          == ME_E_UNSUPPORTED);
    CHECK(err.find("scheme not supported") != std::string::npos);
}

TEST_CASE("decode_sticker_to_rgba8: http(s) scheme accepted (passes to libavformat)") {
    /* The URI scheme guard accepts http:// + https://; unreachable
     * URLs surface as ME_E_IO from avformat_open_input rather than
     * ME_E_UNSUPPORTED at the scheme-check stage. We use a blackhole
     * port (TEST-NET-1 RFC 5737) so we don't depend on DNS state +
     * the failure is fast. */
    me::compose::StickerImage out;
    std::string err;
    /* 192.0.2.0/24 is RFC 5737 reserved for documentation — IANA
     * promises it never resolves. The actual error code from
     * libavformat depends on the platform's network stack
     * (connection refused / unreachable / timeout); the scheme
     * guard's contract is just that it's NOT ME_E_UNSUPPORTED
     * with the "scheme not supported" diag. */
    const me_status_t s = me::compose::decode_sticker_to_rgba8(
        "http://192.0.2.1/sticker.png", &out, &err);
    CHECK(s != ME_E_UNSUPPORTED);
    if (s == ME_E_UNSUPPORTED) {
        /* Diagnostic: surface the err string so a regression
         * (scheme guard accidentally re-rejects http) is debuggable. */
        MESSAGE("err was: " << err);
    }
}

TEST_CASE("resolve_landmark_bboxes_from_file: closest-frame selection") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_face_sticker_landmarks.json";
    guard.paths.push_back(json_path);
    {
        std::ofstream f(json_path);
        f << R"({
            "frames": [
              { "t": {"num": 0,  "den": 30}, "bboxes": [{"x0": 10, "y0": 20, "x1": 30, "y1": 40}] },
              { "t": {"num": 30, "den": 30}, "bboxes": [{"x0": 50, "y0": 60, "x1": 70, "y1": 80},
                                                          {"x0": 90, "y0": 100, "x1": 110, "y1": 120}] },
              { "t": {"num": 60, "den": 30}, "bboxes": [] }
            ]
        })";
    }

    /* Time 0 → first frame (1 bbox). */
    std::vector<me::compose::Bbox> bboxes;
    std::string err;
    REQUIRE_MESSAGE(
        me::compose::resolve_landmark_bboxes_from_file(
            "file://" + json_path, me_rational_t{0, 30}, &bboxes, &err)
            == ME_OK,
        err);
    REQUIRE(bboxes.size() == 1);
    CHECK(bboxes[0].x0 == 10);
    CHECK(bboxes[0].y0 == 20);
    CHECK(bboxes[0].x1 == 30);
    CHECK(bboxes[0].y1 == 40);

    /* Time 1.0s (30/30) → second frame (2 bboxes). */
    REQUIRE(me::compose::resolve_landmark_bboxes_from_file(
                "file://" + json_path, me_rational_t{30, 30}, &bboxes, &err)
            == ME_OK);
    REQUIRE(bboxes.size() == 2);
    CHECK(bboxes[1].x0 == 90);

    /* Time 0.7s (close to second frame) → second frame. */
    REQUIRE(me::compose::resolve_landmark_bboxes_from_file(
                "file://" + json_path, me_rational_t{21, 30}, &bboxes, &err)
            == ME_OK);
    REQUIRE(bboxes.size() == 2);  /* Frame at t=30/30 wins by proximity. */

    /* Time 2.0s (close to third frame) → empty bboxes (legitimate). */
    REQUIRE(me::compose::resolve_landmark_bboxes_from_file(
                "file://" + json_path, me_rational_t{60, 30}, &bboxes, &err)
            == ME_OK);
    CHECK(bboxes.empty());
}

TEST_CASE("resolve_landmark_bboxes_from_file: empty frames array → ME_OK no bboxes") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_face_sticker_empty.json";
    guard.paths.push_back(json_path);
    {
        std::ofstream f(json_path);
        f << R"({"frames": []})";
    }
    std::vector<me::compose::Bbox> bboxes{ {1, 2, 3, 4} };  /* sentinel */
    std::string err;
    REQUIRE(me::compose::resolve_landmark_bboxes_from_file(
                "file://" + json_path, me_rational_t{0, 1}, &bboxes, &err)
            == ME_OK);
    CHECK(bboxes.empty());  /* Cleared even though `frames` is empty. */
}

TEST_CASE("resolve_landmark_bboxes_from_file: malformed JSON → ME_E_PARSE") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_face_sticker_bad.json";
    guard.paths.push_back(json_path);
    {
        std::ofstream f(json_path);
        f << "not json at all";
    }
    std::vector<me::compose::Bbox> bboxes;
    std::string err;
    CHECK(me::compose::resolve_landmark_bboxes_from_file(
              "file://" + json_path, me_rational_t{0, 1}, &bboxes, &err)
          == ME_E_PARSE);
    CHECK(err.find("json parse") != std::string::npos);
}

TEST_CASE("face_sticker compose stage: decode + resolve + apply end-to-end") {
    TempScrub guard;
    const std::string png_path  = "/tmp/me_test_face_sticker_e2e.png";
    const std::string json_path = "/tmp/me_test_face_sticker_e2e.json";
    guard.paths.push_back(png_path);
    guard.paths.push_back(json_path);

    /* Sticker: 8x8 solid green. */
    REQUIRE(encode_solid_png(png_path, 8, 8, 0x00, 0xFF, 0x00, 0xFF));

    /* Landmarks: one bbox at (16, 16)..(48, 48). */
    {
        std::ofstream f(json_path);
        f << R"({
            "frames": [
              { "t": {"num": 0, "den": 30},
                "bboxes": [{"x0": 16, "y0": 16, "x1": 48, "y1": 48}] }
            ]
        })";
    }

    /* Resolve sticker pixels. */
    me::compose::StickerImage sticker;
    std::string err;
    REQUIRE(me::compose::decode_sticker_to_rgba8(
                "file://" + png_path, &sticker, &err) == ME_OK);
    REQUIRE(sticker.width == 8);

    /* Resolve landmarks. */
    std::vector<me::compose::Bbox> bboxes;
    REQUIRE(me::compose::resolve_landmark_bboxes_from_file(
                "file://" + json_path, me_rational_t{0, 30},
                &bboxes, &err) == ME_OK);
    REQUIRE(bboxes.size() == 1);

    /* Black 64x64 RGBA8 canvas (alpha=0xFF so blend has something
     * to blend INTO). */
    std::vector<std::uint8_t> canvas(64 * 64 * 4, 0);
    for (std::size_t i = 3; i < canvas.size(); i += 4) canvas[i] = 0xFF;

    /* Identity-shape params: scale 1.0, offset 0. */
    me::FaceStickerEffectParams p;
    p.landmark.asset_id = "test";
    p.sticker_uri       = "file://" + png_path;
    p.scale_x = 1.0;
    p.scale_y = 1.0;

    REQUIRE(me::compose::apply_face_sticker_inplace(
                canvas.data(), 64, 64, 64 * 4,
                p,
                std::span<const me::compose::Bbox>(bboxes.data(), bboxes.size()),
                sticker.pixels.data(), sticker.width, sticker.height,
                static_cast<std::size_t>(sticker.width * 4)) == ME_OK);

    /* Pixel inside bbox (e.g. 32, 32) should now be green; outside
     * (e.g. 0, 0) should still be black. */
    auto rgba_at = [&](int x, int y) {
        const std::size_t idx = (y * 64 + x) * 4;
        return std::array<std::uint8_t, 4>{
            canvas[idx + 0], canvas[idx + 1],
            canvas[idx + 2], canvas[idx + 3]
        };
    };

    auto inside = rgba_at(32, 32);
    CHECK(inside[1] == 0xFF);  /* G channel */
    CHECK(inside[0] == 0x00);

    auto outside = rgba_at(0, 0);
    CHECK(outside[0] == 0x00);
    CHECK(outside[1] == 0x00);
    CHECK(outside[2] == 0x00);
}
