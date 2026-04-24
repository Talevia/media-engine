/*
 * test_effect_chain — contract pins for me::effect::EffectChain
 * + IdentityEffect.
 *
 * Exercises: empty chain is a no-op, Identity effect preserves
 * bytes exactly, chain iterates effects in declaration order,
 * kind_at introspection, out-of-range indexing returns nullptr.
 */
#include <doctest/doctest.h>

#include "effect/effect.hpp"
#include "effect/effect_chain.hpp"
#include "effect/identity_effect.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using me::effect::Effect;
using me::effect::EffectChain;
using me::effect::IdentityEffect;

namespace {

/* Test-only effect that writes its `fill` byte into every RGBA
 * channel (A + R + G + B) of every pixel. Used to prove chain
 * iteration order — later effects overwrite earlier ones. */
class FillEffect final : public Effect {
public:
    explicit FillEffect(std::uint8_t fill) : fill_(fill) {}
    void apply(std::uint8_t* rgba, int w, int h, std::size_t stride) const override {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = rgba + y * stride + x * 4;
                p[0] = fill_; p[1] = fill_; p[2] = fill_; p[3] = fill_;
            }
        }
    }
    const char* kind() const noexcept override { return "test_fill"; }
private:
    std::uint8_t fill_;
};

}  // namespace

TEST_CASE("EffectChain: empty chain preserves buffer bytes exactly") {
    const int W = 4, H = 4;
    std::vector<std::uint8_t> before(W * H * 4);
    for (std::size_t i = 0; i < before.size(); ++i) before[i] = static_cast<std::uint8_t>(i);
    std::vector<std::uint8_t> buf = before;

    EffectChain chain;
    CHECK(chain.empty());
    CHECK(chain.size() == 0);

    chain.apply(buf.data(), W, H, static_cast<std::size_t>(W) * 4);
    CHECK(buf == before);
}

TEST_CASE("EffectChain: IdentityEffect leaves buffer bytes exactly unchanged") {
    const int W = 8, H = 8;
    std::vector<std::uint8_t> before(W * H * 4);
    for (std::size_t i = 0; i < before.size(); ++i) before[i] = static_cast<std::uint8_t>(i * 3);
    std::vector<std::uint8_t> buf = before;

    EffectChain chain;
    chain.append(std::make_unique<IdentityEffect>());
    REQUIRE(chain.size() == 1);
    CHECK(std::string{chain.kind_at(0)} == "identity");

    chain.apply(buf.data(), W, H, static_cast<std::size_t>(W) * 4);
    CHECK(buf == before);
}

TEST_CASE("EffectChain: effects applied in declaration order (later overwrites earlier)") {
    const int W = 2, H = 2;
    std::vector<std::uint8_t> buf(W * H * 4, 0);

    EffectChain chain;
    chain.append(std::make_unique<FillEffect>(0xAA));   /* first */
    chain.append(std::make_unique<FillEffect>(0xBB));   /* second */
    REQUIRE(chain.size() == 2);
    CHECK(std::string{chain.kind_at(0)} == "test_fill");
    CHECK(std::string{chain.kind_at(1)} == "test_fill");

    chain.apply(buf.data(), W, H, static_cast<std::size_t>(W) * 4);
    /* Final state = last FillEffect's fill byte. */
    for (std::uint8_t b : buf) CHECK(b == 0xBB);
}

TEST_CASE("EffectChain: Identity between two FillEffects does not disturb chain order") {
    const int W = 1, H = 1;
    std::vector<std::uint8_t> buf(W * H * 4, 0);

    EffectChain chain;
    chain.append(std::make_unique<FillEffect>(0x10));
    chain.append(std::make_unique<IdentityEffect>());
    chain.append(std::make_unique<FillEffect>(0x20));
    REQUIRE(chain.size() == 3);

    chain.apply(buf.data(), W, H, static_cast<std::size_t>(W) * 4);
    /* Identity in the middle is a pass-through; last Fill wins. */
    for (std::uint8_t b : buf) CHECK(b == 0x20);
}

TEST_CASE("EffectChain: kind_at out-of-range returns nullptr") {
    EffectChain chain;
    chain.append(std::make_unique<IdentityEffect>());
    CHECK(chain.kind_at(0) != nullptr);
    CHECK(chain.kind_at(1) == nullptr);
    CHECK(chain.kind_at(99) == nullptr);
}

TEST_CASE("EffectChain: move-only ownership transfers effects correctly") {
    EffectChain src;
    src.append(std::make_unique<IdentityEffect>());
    src.append(std::make_unique<FillEffect>(0x77));
    REQUIRE(src.size() == 2);

    EffectChain dst = std::move(src);
    CHECK(dst.size() == 2);
    CHECK(std::string{dst.kind_at(0)} == "identity");
    CHECK(std::string{dst.kind_at(1)} == "test_fill");

    /* src moved-from — its effects vector is empty. */
    CHECK(src.size() == 0);

    /* dst still fully functional. */
    const int W = 1, H = 1;
    std::vector<std::uint8_t> buf(4, 0);
    dst.apply(buf.data(), W, H, 4);
    for (std::uint8_t b : buf) CHECK(b == 0x77);
}
