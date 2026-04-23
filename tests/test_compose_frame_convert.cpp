/*
 * test_compose_frame_convert — numerical + lifecycle tripwire for
 * me::compose::frame_to_rgba8 / rgba8_to_frame.
 *
 * What this suite pins:
 *   - Happy path YUV444P → RGBA8 produces expected output size and
 *     reasonable pixel values (white-source → RGBA near white).
 *   - Round trip YUV444P → RGBA8 → YUV444P stays within a small
 *     integer tolerance (chroma-subsample-free format, so loss is
 *     only from sws rounding).
 *   - Null / zero-dim arguments rejected with ME_E_INVALID_ARG.
 *   - Dimension-mismatched dst AVFrame rejected.
 *
 * The helpers run `sws_scale` at matching dimensions (no geometric
 * scaling), so the pixel-format conversion is the only thing being
 * tested; this keeps the numerical tolerance tight.
 */
#include <doctest/doctest.h>

#include "compose/frame_convert.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

/* RAII wrapper for an allocated AVFrame filled with per-pixel Y/U/V
 * values. Caller passes desired YUV format (e.g. AV_PIX_FMT_YUV444P);
 * we fill planes with scalar values. */
struct YuvFrame {
    AVFrame* frame = nullptr;
    ~YuvFrame() { if (frame) av_frame_free(&frame); }
};

YuvFrame make_yuv_frame(int width, int height, AVPixelFormat fmt,
                         uint8_t y_val, uint8_t u_val, uint8_t v_val) {
    YuvFrame f;
    f.frame = av_frame_alloc();
    REQUIRE(f.frame != nullptr);
    f.frame->width  = width;
    f.frame->height = height;
    f.frame->format = fmt;
    /* Align 32 — both x86 AVX2 and Apple Silicon NEON prefer 32-byte
     * aligned sws inputs; good-enough default for the tests. */
    REQUIRE(av_frame_get_buffer(f.frame, 32) >= 0);

    /* YUV444P stores Y, U, V as full-resolution planes; Y=0, U=1, V=2.
     * YUV420P same indices but U/V are half-sized — we pick 444P for
     * these tests so the pixel-fill loop stays trivial. */
    for (int y = 0; y < height; ++y) {
        std::memset(f.frame->data[0] + y * f.frame->linesize[0], y_val, width);
        std::memset(f.frame->data[1] + y * f.frame->linesize[1], u_val, width);
        std::memset(f.frame->data[2] + y * f.frame->linesize[2], v_val, width);
    }
    return f;
}

}  // namespace

TEST_CASE("frame_to_rgba8: null src returns ME_E_INVALID_ARG") {
    std::vector<uint8_t> buf;
    std::string err;
    const me_status_t s = me::compose::frame_to_rgba8(nullptr, buf, &err);
    CHECK(s == ME_E_INVALID_ARG);
    CHECK(buf.empty());
    CHECK(!err.empty());
}

TEST_CASE("frame_to_rgba8: zero-dim src returns ME_E_INVALID_ARG") {
    auto f = make_yuv_frame(8, 8, AV_PIX_FMT_YUV444P, 0, 128, 128);
    f.frame->width = 0;   /* zero dim */
    std::vector<uint8_t> buf;
    const me_status_t s = me::compose::frame_to_rgba8(f.frame, buf, nullptr);
    CHECK(s == ME_E_INVALID_ARG);
}

TEST_CASE("frame_to_rgba8: white YUV444P input produces near-white RGBA8 output") {
    /* YUV full-range-ish: Y=235, U=128, V=128 → BT.601 yields ~white.
     * sws_scale uses the limited-range BT.601 by default unless
     * configured otherwise; Y=235 is the white point in limited
     * range, so the resulting RGB should be near 255. */
    auto f = make_yuv_frame(16, 16, AV_PIX_FMT_YUV444P, 235, 128, 128);
    std::vector<uint8_t> rgba;
    REQUIRE(me::compose::frame_to_rgba8(f.frame, rgba, nullptr) == ME_OK);
    REQUIRE(rgba.size() == 16u * 16u * 4u);

    /* Spot-check center pixel: should be near white within sws
     * precision (~±5). Alpha must be 255 (YUV has no alpha source). */
    const size_t center = (8u * 16u + 8u) * 4u;
    CHECK(rgba[center + 0] >= 245);
    CHECK(rgba[center + 1] >= 245);
    CHECK(rgba[center + 2] >= 245);
    CHECK(rgba[center + 3] == 255);
}

TEST_CASE("frame_to_rgba8: black YUV444P produces near-black RGBA8") {
    /* Y=16 is black in limited range. */
    auto f = make_yuv_frame(8, 8, AV_PIX_FMT_YUV444P, 16, 128, 128);
    std::vector<uint8_t> rgba;
    REQUIRE(me::compose::frame_to_rgba8(f.frame, rgba, nullptr) == ME_OK);
    const size_t c = (4u * 8u + 4u) * 4u;
    CHECK(rgba[c + 0] <= 10);
    CHECK(rgba[c + 1] <= 10);
    CHECK(rgba[c + 2] <= 10);
    CHECK(rgba[c + 3] == 255);
}

TEST_CASE("rgba8_to_frame: null args rejected") {
    AVFrame* f = av_frame_alloc();
    f->width = 8; f->height = 8; f->format = AV_PIX_FMT_YUV444P;
    REQUIRE(av_frame_get_buffer(f, 32) >= 0);

    std::vector<uint8_t> rgba(8u * 8u * 4u, 255);

    std::string err;
    CHECK(me::compose::rgba8_to_frame(nullptr, 8, 8, 32, f, &err) == ME_E_INVALID_ARG);
    err.clear();
    CHECK(me::compose::rgba8_to_frame(rgba.data(), 8, 8, 32, nullptr, &err) == ME_E_INVALID_ARG);

    av_frame_free(&f);
}

TEST_CASE("rgba8_to_frame: dimension mismatch rejected") {
    AVFrame* f = av_frame_alloc();
    f->width = 8; f->height = 8; f->format = AV_PIX_FMT_YUV444P;
    REQUIRE(av_frame_get_buffer(f, 32) >= 0);

    std::vector<uint8_t> rgba(16u * 16u * 4u, 255);
    std::string err;
    CHECK(me::compose::rgba8_to_frame(rgba.data(), 16, 16, 16 * 4, f, &err)
          == ME_E_INVALID_ARG);
    CHECK(err.find("don't match") != std::string::npos);

    av_frame_free(&f);
}

TEST_CASE("rgba8_to_frame: stride too small rejected") {
    AVFrame* f = av_frame_alloc();
    f->width = 8; f->height = 8; f->format = AV_PIX_FMT_YUV444P;
    REQUIRE(av_frame_get_buffer(f, 32) >= 0);

    std::vector<uint8_t> rgba(8u * 8u * 4u, 255);
    std::string err;
    /* Claim stride 16 but rgba is really 32 per row — reject. */
    CHECK(me::compose::rgba8_to_frame(rgba.data(), 8, 8, 16, f, &err)
          == ME_E_INVALID_ARG);
    CHECK(err.find("smaller than") != std::string::npos);

    av_frame_free(&f);
}

TEST_CASE("round-trip YUV444P → RGBA8 → YUV444P within 4 LSB (no chroma subsample)") {
    /* Known non-trivial pixel: mid-gray + strong chroma. Precisely
     * representable in YUV444P (no subsample loss); round trip through
     * RGBA8 only costs ≤ ~2 LSB from sws rounding per channel. */
    const uint8_t Y = 180, U = 90, V = 170;
    auto src = make_yuv_frame(16, 16, AV_PIX_FMT_YUV444P, Y, U, V);

    std::vector<uint8_t> rgba;
    REQUIRE(me::compose::frame_to_rgba8(src.frame, rgba, nullptr) == ME_OK);

    AVFrame* dst = av_frame_alloc();
    dst->width = 16; dst->height = 16; dst->format = AV_PIX_FMT_YUV444P;
    REQUIRE(av_frame_get_buffer(dst, 32) >= 0);

    REQUIRE(me::compose::rgba8_to_frame(rgba.data(), 16, 16, 16 * 4, dst, nullptr)
            == ME_OK);

    /* Spot-check a center region; outside a small border sws scaling
     * filter doesn't bleed. */
    const int cy = 8, cx = 8;
    const int ry  = dst->data[0][cy * dst->linesize[0] + cx];
    const int ru  = dst->data[1][cy * dst->linesize[1] + cx];
    const int rv  = dst->data[2][cy * dst->linesize[2] + cx];
    const int tol = 4;
    CHECK(std::abs(ry - Y) <= tol);
    CHECK(std::abs(ru - U) <= tol);
    CHECK(std::abs(rv - V) <= tol);

    av_frame_free(&dst);
}

TEST_CASE("frame_to_rgba8: output is byte-deterministic across repeated calls") {
    /* Same frame → sws output must not drift. Guards against future
     * SWS_FAST_BILINEAR / SIMD-dispatch flip that would break the
     * byte-deterministic software path (VISION §5.3). */
    auto f = make_yuv_frame(32, 32, AV_PIX_FMT_YUV444P, 150, 80, 200);
    std::vector<uint8_t> a, b;
    REQUIRE(me::compose::frame_to_rgba8(f.frame, a, nullptr) == ME_OK);
    REQUIRE(me::compose::frame_to_rgba8(f.frame, b, nullptr) == ME_OK);
    CHECK(a == b);
}
