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
#include <memory>
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
TEST_CASE("OcioPipeline returns ME_E_UNSUPPORTED on non-identity pair") {
    /* Skeletal OCIO wiring returns UNSUPPORTED for any pair the
     * identity fast-path doesn't cover. The real bt709 ↔ sRGB ↔
     * linear conversions land with the ocio-colorspace-conversions
     * backlog bullet. Contract pinned here so the test flips the
     * moment that cycle upgrades to a real transform. */
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    me::ColorSpace src{};
    src.primaries = me::ColorSpace::Primaries::BT709;
    src.transfer  = me::ColorSpace::Transfer::BT709;

    me::ColorSpace dst = src;
    dst.transfer = me::ColorSpace::Transfer::SRGB;   /* only axis differs */

    std::vector<uint8_t> buf(64, 0);
    std::string err;
    const me_status_t s = p->apply(buf.data(), buf.size(), src, dst, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("non-identity colorspace conversion") != std::string::npos);
    CHECK(err.find("transfer") != std::string::npos);
}
#endif  /* ME_HAS_OCIO */
