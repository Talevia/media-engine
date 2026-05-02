/*
 * test_mask_resolver — coverage for
 * `me::compose::resolve_mask_alpha_from_file` (M11
 * `body-alpha-key-mask-resolver-impl`).
 *
 * Mirrors test_face_sticker_compose_stage's pattern:
 *   - Encode a tiny mask alpha buffer to base64 at runtime.
 *   - Write a JSON fixture next to the test.
 *   - Call resolve_mask_alpha_from_file; assert dimensions +
 *     pixel-byte round-trip.
 *
 * Coverage:
 *   - Round-trips an 8x8 alpha buffer (write + read + compare).
 *   - Closest-frame selection: 3 frames at t=0/30, 30/30, 60/30,
 *     query at 0, 30/30, 21/30 (close to 30/30) — assert correct
 *     frame chosen.
 *   - Empty `frames` array → ME_OK with width/height/alpha all
 *     zero-shaped.
 *   - Malformed JSON → ME_E_PARSE.
 *   - Decoded byte count != width*height → ME_E_PARSE with diag.
 *   - Empty URI / bad scheme rejected.
 */
#include <doctest/doctest.h>

#include "compose/mask_resolver.hpp"

extern "C" {
#include <libavutil/base64.h>
}

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct TempScrub {
    std::vector<std::string> paths;
    ~TempScrub() { for (const auto& p : paths) std::remove(p.c_str()); }
};

/* Encode `bytes` as base64 via libavutil. Returns the b64 string
 * (NUL-terminated stripped). */
std::string b64_encode(const std::vector<std::uint8_t>& bytes) {
    /* AV_BASE64_SIZE is `((in_size + 2) / 3) * 4 + 1`. */
    const std::size_t out_size = ((bytes.size() + 2) / 3) * 4 + 1;
    std::vector<char> buf(out_size);
    char* p = av_base64_encode(buf.data(),
                                static_cast<int>(buf.size()),
                                bytes.data(),
                                static_cast<int>(bytes.size()));
    if (!p) return std::string();
    return std::string(buf.data());
}

}  // namespace

TEST_CASE("resolve_mask_alpha_from_file: round-trips an 8x8 alpha buffer") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_mask_resolver_8x8.json";
    guard.paths.push_back(json_path);

    /* Synthetic alpha: gradient 0..63 row-major. */
    std::vector<std::uint8_t> alpha(8 * 8);
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        alpha[i] = static_cast<std::uint8_t>(i);
    }
    {
        std::ofstream f(json_path);
        f << R"({"frames":[{"t":{"num":0,"den":30},"width":8,"height":8,"alphaB64":")"
          << b64_encode(alpha) << R"("}]})";
    }

    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;
    REQUIRE_MESSAGE(
        me::compose::resolve_mask_alpha_from_file(
            "file://" + json_path, me_rational_t{0, 30},
            &w, &h, &got, &err) == ME_OK,
        err);
    CHECK(w == 8);
    CHECK(h == 8);
    REQUIRE(got.size() == 64);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == static_cast<std::uint8_t>(i));
    }
}

TEST_CASE("resolve_mask_alpha_from_file: closest-frame selection") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_mask_resolver_3frames.json";
    guard.paths.push_back(json_path);

    auto solid = [](int w, int h, std::uint8_t v) {
        return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, v);
    };

    /* Three distinct fills so we can identify which frame got
     * picked: 0x10, 0x20, 0x30. */
    std::vector<std::uint8_t> a = solid(4, 4, 0x10);
    std::vector<std::uint8_t> b = solid(4, 4, 0x20);
    std::vector<std::uint8_t> c = solid(4, 4, 0x30);
    {
        std::ofstream f(json_path);
        f << R"({"frames":[
            {"t":{"num":0,"den":30}, "width":4,"height":4,"alphaB64":")" << b64_encode(a) << R"("},
            {"t":{"num":30,"den":30},"width":4,"height":4,"alphaB64":")" << b64_encode(b) << R"("},
            {"t":{"num":60,"den":30},"width":4,"height":4,"alphaB64":")" << b64_encode(c) << R"("}
        ]})";
    }

    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;

    REQUIRE(me::compose::resolve_mask_alpha_from_file(
                "file://" + json_path, me_rational_t{0, 30},
                &w, &h, &got, &err) == ME_OK);
    CHECK(got[0] == 0x10);

    REQUIRE(me::compose::resolve_mask_alpha_from_file(
                "file://" + json_path, me_rational_t{30, 30},
                &w, &h, &got, &err) == ME_OK);
    CHECK(got[0] == 0x20);

    /* t = 21/30 is closer to 30/30 than 0/30 — pick frame 1. */
    REQUIRE(me::compose::resolve_mask_alpha_from_file(
                "file://" + json_path, me_rational_t{21, 30},
                &w, &h, &got, &err) == ME_OK);
    CHECK(got[0] == 0x20);

    REQUIRE(me::compose::resolve_mask_alpha_from_file(
                "file://" + json_path, me_rational_t{60, 30},
                &w, &h, &got, &err) == ME_OK);
    CHECK(got[0] == 0x30);
}

TEST_CASE("resolve_mask_alpha_from_file: empty frames array → zero-shaped output") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_mask_resolver_empty.json";
    guard.paths.push_back(json_path);
    { std::ofstream f(json_path); f << R"({"frames":[]})"; }

    int w = 99, h = 99;
    std::vector<std::uint8_t> got{ 1, 2, 3 };  /* sentinel */
    std::string err;
    REQUIRE(me::compose::resolve_mask_alpha_from_file(
                "file://" + json_path, me_rational_t{0, 1},
                &w, &h, &got, &err) == ME_OK);
    CHECK(w == 0);
    CHECK(h == 0);
    CHECK(got.empty());
}

TEST_CASE("resolve_mask_alpha_from_file: malformed JSON → ME_E_PARSE") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_mask_resolver_bad.json";
    guard.paths.push_back(json_path);
    { std::ofstream f(json_path); f << "not json"; }

    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_from_file(
              "file://" + json_path, me_rational_t{0, 1},
              &w, &h, &got, &err) == ME_E_PARSE);
    CHECK(err.find("json parse") != std::string::npos);
}

TEST_CASE("resolve_mask_alpha_from_file: byte count mismatch → ME_E_PARSE") {
    TempScrub guard;
    const std::string json_path = "/tmp/me_test_mask_resolver_mismatch.json";
    guard.paths.push_back(json_path);

    /* 4-byte payload but width*height = 16 → mismatch. */
    std::vector<std::uint8_t> too_few = { 0, 0, 0, 0 };
    {
        std::ofstream f(json_path);
        f << R"({"frames":[{"t":{"num":0,"den":1},"width":4,"height":4,"alphaB64":")"
          << b64_encode(too_few) << R"("}]})";
    }

    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_from_file(
              "file://" + json_path, me_rational_t{0, 1},
              &w, &h, &got, &err) == ME_E_PARSE);
    CHECK(err.find("decoded") != std::string::npos);
    CHECK(err.find("expected 16") != std::string::npos);
}

TEST_CASE("resolve_mask_alpha_from_file: empty URI rejected") {
    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_from_file(
              "", me_rational_t{0, 1}, &w, &h, &got, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("resolve_mask_alpha_from_file: unsupported scheme rejected") {
    int w = 0, h = 0;
    std::vector<std::uint8_t> got;
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_from_file(
              "https://example.com/mask.json",
              me_rational_t{0, 1}, &w, &h, &got, &err)
          == ME_E_UNSUPPORTED);
    CHECK(err.find("scheme not supported") != std::string::npos);
}
