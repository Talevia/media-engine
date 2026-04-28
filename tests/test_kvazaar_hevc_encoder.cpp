/*
 * test_kvazaar_hevc_encoder — unit coverage for
 * `me::io::KvazaarHevcEncoder` (M10 SW HEVC fallback). Synthetic
 * gray-pattern YUV420P frames feed the encoder; the test asserts
 * the produced bitstream parses as HEVC Annex-B (start code
 * + non-empty NALs) and that the rejection paths (resolution
 * cap, non-multiple-of-8 dims, post-EOF send_frame) return the
 * documented error codes.
 *
 * Gated on `ME_HAS_KVAZAAR` — set by tests/CMakeLists.txt's own
 * pkg-config probe (the engine target's compile def is PRIVATE
 * and doesn't propagate through target_link_libraries). Hosts
 * without Kvazaar see a single "skipped" test_case.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_KVAZAAR
#include "io/kvazaar_hevc_encoder.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

/* Build a synthetic gray YUV420P frame at the given resolution.
 * Y = 128 (mid-gray), U = V = 128 (neutral chroma). All planes
 * tightly packed (stride == width). */
struct GrayFrame {
    std::vector<std::uint8_t> y, u, v;
    int w, h;

    GrayFrame(int width, int height) : w(width), h(height) {
        y.assign(static_cast<std::size_t>(width) * height, 128);
        u.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
        v.assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
    }
};

bool starts_with_annexb_startcode(std::span<const std::uint8_t> bytes) {
    /* HEVC NALs in Annex B start with 0x00 0x00 0x00 0x01 or
     * 0x00 0x00 0x01. Either is acceptable. */
    if (bytes.size() < 3) return false;
    if (bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x01) return true;
    if (bytes.size() >= 4 &&
        bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x00 &&
        bytes[3] == 0x01) return true;
    return false;
}

}  // namespace

TEST_CASE("KvazaarHevcEncoder: 64x64 gray frames produce HEVC Annex-B output") {
    std::string err;
    auto enc = me::io::KvazaarHevcEncoder::create(
        64, 64, 30, 1, 0, &err);
    REQUIRE_MESSAGE(enc != nullptr, err);

    /* Encode 4 frames. Kvazaar's lookahead is 0 (--owf=0) so each
     * frame either emits packets immediately or queues at most
     * one frame's worth in the reorder buffer. */
    std::vector<std::uint8_t> all_bytes;
    auto on_packet = [&](std::span<const std::uint8_t> bytes) {
        all_bytes.insert(all_bytes.end(), bytes.begin(), bytes.end());
    };

    for (int i = 0; i < 4; ++i) {
        GrayFrame f{64, 64};
        const std::int64_t pts_us = i * 33333;  /* 30 fps */
        REQUIRE(enc->send_frame(
            f.y.data(), 64,
            f.u.data(), 32,
            f.v.data(), 32,
            pts_us, &err) == ME_OK);
        REQUIRE(enc->flush_packets(on_packet, &err) == ME_OK);
    }

    /* Drain the reorder buffer. */
    enc->send_eof();
    REQUIRE(enc->flush_packets(on_packet, &err) == ME_OK);

    REQUIRE(all_bytes.size() > 0);
    /* The first packet of the stream must lead with VPS/SPS/PPS
     * NALs and start with an Annex-B start code. */
    CHECK(starts_with_annexb_startcode(
        std::span<const std::uint8_t>(all_bytes.data(), all_bytes.size())));
}

TEST_CASE("KvazaarHevcEncoder: rejects > 1080p with named diag") {
    std::string err;
    auto enc = me::io::KvazaarHevcEncoder::create(
        1920, 1200, 30, 1, 0, &err);
    CHECK(enc == nullptr);
    CHECK(err.find("1920x1080") != std::string::npos);
}

TEST_CASE("KvazaarHevcEncoder: rejects width that isn't multiple of 8") {
    std::string err;
    auto enc = me::io::KvazaarHevcEncoder::create(
        100, 64, 30, 1, 0, &err);
    CHECK(enc == nullptr);
    CHECK(err.find("multiples of 8") != std::string::npos);
}

TEST_CASE("KvazaarHevcEncoder: rejects non-positive dimensions") {
    std::string err;
    auto a = me::io::KvazaarHevcEncoder::create(0, 64, 30, 1, 0, &err);
    CHECK(a == nullptr);
    CHECK(!err.empty());

    err.clear();
    auto b = me::io::KvazaarHevcEncoder::create(64, -1, 30, 1, 0, &err);
    CHECK(b == nullptr);
    CHECK(!err.empty());
}

TEST_CASE("KvazaarHevcEncoder: send_frame after send_eof returns ME_E_INVALID_ARG") {
    std::string err;
    auto enc = me::io::KvazaarHevcEncoder::create(
        64, 64, 30, 1, 0, &err);
    REQUIRE(enc != nullptr);

    enc->send_eof();

    GrayFrame f{64, 64};
    const me_status_t rc = enc->send_frame(
        f.y.data(), 64,
        f.u.data(), 32,
        f.v.data(), 32,
        0, &err);
    CHECK(rc == ME_E_INVALID_ARG);
    CHECK(err.find("EOF") != std::string::npos);
}

TEST_CASE("KvazaarHevcEncoder: at exactly 1920x1080 (cap boundary) succeeds") {
    /* Boundary check — the cap is `> 1920x1080`, so exactly the
     * 1920×1080 frame must succeed. Don't actually encode a frame
     * here (1080p encode is slow); construction-only verifies the
     * dimension validation logic. */
    std::string err;
    auto enc = me::io::KvazaarHevcEncoder::create(
        1920, 1080, 30, 1, 0, &err);
    REQUIRE_MESSAGE(enc != nullptr, err);
}

#else  /* !ME_HAS_KVAZAAR */

TEST_CASE("KvazaarHevcEncoder: skipped (ME_WITH_KVAZAAR=OFF or pkg-config missing)") {
    /* Build-flag-gated stub. */
}

#endif
