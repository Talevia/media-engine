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

TEST_CASE("IdentityPipeline::apply is a no-op returning ME_OK") {
    auto p = me::color::make_pipeline();
    REQUIRE(p != nullptr);

    /* bt709 limited → bt709 limited: identity transform semantically,
     * and the concrete IdentityPipeline just returns ME_OK. */
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
