/*
 * test_color_pipeline — smoke tests for me::color::make_pipeline().
 *
 * The factory lives in a header-only stub (`src/color/pipeline.hpp`)
 * that has no consumer inside media_engine today, so without a test
 * that calls into it, the `inline make_pipeline()` body never gets
 * instantiated and the preprocessor branch / return type stays
 * compile-unchecked. This suite forces both default path instantiation
 * and the IdentityPipeline apply contract to participate in the build.
 *
 * Real color-math coverage arrives with `OcioPipeline`; until then
 * this suite is a deliberate "factory + identity contract" tripwire.
 */
#include <doctest/doctest.h>

#include "color/pipeline.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstdint>
#include <cstdlib>   /* setenv / unsetenv / getenv for OCIO env tests */
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("me::color::make_pipeline returns a non-null Pipeline") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);
}

TEST_CASE("Pipeline::apply is a no-op returning ME_OK on src == dst") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    /* bt709 limited → bt709 limited: identity transform. Both the
     * IdentityPipeline (ME_WITH_OCIO=OFF) and OcioPipeline
     * (ME_WITH_OCIO=ON) fast-path this case. */
    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;
    src.matrix    = me::ColorSpace::Matrix::BT709;
    src.range     = me::ColorSpace::Range::Limited;

    me::ColorSpace dst = src;

    std::vector<uint8_t> buf(64, 0x7f);
    const std::vector<uint8_t> copy_before = buf;
    std::string err;

    const me_status_t s = p->apply(buf.data(), buf.size(), src, dst, &err);
    CHECK(s == ME_OK);
    CHECK(err.empty());
    /* Identity path must not mutate the buffer. */
    CHECK(buf == copy_before);
}

#if ME_HAS_OCIO
TEST_CASE("OcioPipeline: bt709 → sRGB transfer conversion modifies buffer") {
    /* bt709 transfer (BT.1886 gamma ~2.4) vs sRGB (piecewise EOTF)
     * differ by up to a few LSB on midtones — OCIO applies the
     * curve difference in-place. Pin that the buffer is actually
     * touched (not all-zero no-op). */
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.transfer = me::ColorSpace::Transfer::SRGB;   /* only axis differs */

    /* Midgray RGBA pattern where sRGB ≠ BT.1886 values noticeably. */
    std::vector<uint8_t> buf(16, 128);   /* 4 RGBA pixels @ 128 per channel */
    const std::vector<uint8_t> before = buf;
    std::string err;
    const me_status_t s = p->apply(buf.data(), buf.size(), src, dst, &err);
    CHECK(s == ME_OK);
    CHECK(err.empty());
    /* At least one byte in the RGB channels should differ (alpha is
     * passed through unchanged). */
    bool any_rgb_diff = false;
    for (size_t i = 0; i < buf.size(); ++i) {
        if ((i % 4) == 3) continue;   /* skip alpha */
        if (buf[i] != before[i]) { any_rgb_diff = true; break; }
    }
    CHECK(any_rgb_diff);
}

TEST_CASE("OcioPipeline: bt709 → linear → bt709 round-trip within tolerance") {
    /* 8-bit quantised round trip loses some precision (< ~4 LSB on
     * typical midtone values). Pin an approximate identity so future
     * config changes that break the curves get caught. */
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace bt709{};
    bt709.primaries = me::ColorSpace::Primaries::BT709;
    bt709.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace linear = bt709;
    linear.transfer = me::ColorSpace::Transfer::Linear;

    /* 64 pixels of a midtone gradient. */
    std::vector<uint8_t> buf(256);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i % 200 + 30);
    const std::vector<uint8_t> original = buf;

    std::string err;
    const me_status_t s1 = p->apply(buf.data(), buf.size(), bt709, linear, &err);
    if (s1 != ME_OK) MESSAGE("bt709→linear failed: " << err);
    REQUIRE(s1 == ME_OK);
    err.clear();
    const me_status_t s2 = p->apply(buf.data(), buf.size(), linear, bt709, &err);
    if (s2 != ME_OK) MESSAGE("linear→bt709 failed: " << err);
    REQUIRE(s2 == ME_OK);

    int max_diff = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if ((i % 4) == 3) continue;   /* alpha unchanged */
        max_diff = std::max(max_diff, std::abs(int(buf[i]) - int(original[i])));
    }
    /* 8-bit round-trip through log-ish → linear → log-ish accumulates
     * ~few LSB of quantisation error. 6 is a loose-but-real bound. */
    CHECK(max_diff <= 6);
}

TEST_CASE("OcioPipeline: BT2020 primaries not mapped, returns ME_E_UNSUPPORTED") {
    /* Phase-1 only supports BT.709 primaries. BT2020 (for HDR) is a
     * separate future milestone (M8+ HDR). Pin the explicit rejection. */
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.primaries = me::ColorSpace::Primaries::BT2020;

    std::vector<uint8_t> buf(16, 0);
    std::string err;
    const me_status_t s = p->apply(buf.data(), buf.size(), src, dst, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("no OCIO role mapping") != std::string::npos);
}

TEST_CASE("OcioPipeline: PQ transfer not mapped, returns ME_E_UNSUPPORTED") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.transfer = me::ColorSpace::Transfer::PQ;

    std::vector<uint8_t> buf(16, 0);
    std::string err;
    CHECK(p->apply(buf.data(), buf.size(), src, dst, &err) == ME_E_UNSUPPORTED);
}

TEST_CASE("OcioPipeline: byte_count not multiple of 4 returns ME_E_INVALID_ARG") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.transfer = me::ColorSpace::Transfer::SRGB;

    std::vector<uint8_t> buf(15, 0);   /* not a multiple of 4 */
    std::string err;
    CHECK(p->apply(buf.data(), buf.size(), src, dst, &err) == ME_E_INVALID_ARG);
    CHECK(err.find("not a multiple of 4") != std::string::npos);
}

TEST_CASE("OcioPipeline: null buffer for non-identity returns ME_E_INVALID_ARG") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.transfer = me::ColorSpace::Transfer::SRGB;

    CHECK(p->apply(nullptr, 16, src, dst, nullptr) == ME_E_INVALID_ARG);
}

/* --- M10 OCIO config resolution: explicit path > $OCIO env > builtin --- */

TEST_CASE("OcioPipeline: explicit invalid config_path throws with path mentioned") {
    /* Caller asked for a specific file; no silent fallback to env or
     * builtin. The diagnostic must name the explicit path so the
     * host can fix the typo. */
    bool threw = false;
    std::string what;
    try {
        (void)me::color::make_pipeline("/nonexistent/no-such.ocio");
    } catch (const std::runtime_error& e) {
        threw = true;
        what  = e.what();
    }
    CHECK(threw);
    CHECK(what.find("explicit path") != std::string::npos);
    CHECK(what.find("no-such.ocio") != std::string::npos);
}

TEST_CASE("OcioPipeline: $OCIO env pointing at invalid file throws with env value") {
    /* When `ocio_config_path` is empty but $OCIO is set, that env
     * value is the next resolution step. An invalid file there
     * also throws — the alternative (silent fallback to builtin)
     * would mask a host config bug. */
    const char* prev = std::getenv("OCIO");
    setenv("OCIO", "/nonexistent/env-config.ocio", /*overwrite=*/1);

    bool threw = false;
    std::string what;
    try {
        (void)me::color::make_pipeline(nullptr);
    } catch (const std::runtime_error& e) {
        threw = true;
        what  = e.what();
    }

    /* Restore env regardless of test outcome. */
    if (prev) setenv("OCIO", prev, 1);
    else      unsetenv("OCIO");

    CHECK(threw);
    CHECK(what.find("$OCIO=") != std::string::npos);
    CHECK(what.find("env-config.ocio") != std::string::npos);
}

TEST_CASE("OcioPipeline: explicit config_path takes precedence over $OCIO env") {
    /* Both set, both invalid (different paths) — diagnostic must
     * cite the explicit path, not the env, proving precedence. */
    const char* prev = std::getenv("OCIO");
    setenv("OCIO", "/should/not/be/seen.ocio", 1);

    bool threw = false;
    std::string what;
    try {
        (void)me::color::make_pipeline("/explicit/wins.ocio");
    } catch (const std::runtime_error& e) {
        threw = true;
        what  = e.what();
    }

    if (prev) setenv("OCIO", prev, 1);
    else      unsetenv("OCIO");

    CHECK(threw);
    CHECK(what.find("/explicit/wins.ocio") != std::string::npos);
    CHECK(what.find("should/not/be/seen") == std::string::npos);
}

TEST_CASE("OcioPipeline: NULL config_path + unset $OCIO falls through to builtin") {
    /* Default path — what every existing caller hits. Must continue
     * to succeed (no throw) so the env-or-builtin fallback chain
     * stays compatible with the pre-resolution-order behaviour. */
    const char* prev = std::getenv("OCIO");
    unsetenv("OCIO");

    bool threw = false;
    try {
        auto p = me::color::make_pipeline(nullptr);
        REQUIRE(p != nullptr);
    } catch (...) {
        threw = true;
    }

    if (prev) setenv("OCIO", prev, 1);

    CHECK_FALSE(threw);
}
#endif  /* ME_HAS_OCIO */
