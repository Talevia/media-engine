#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

/* Write a known-bytes file so the subsequent sha256 has a predictable
 * value. Content "abc" hashes to the NIST SHA-256 vector. */
fs::path write_abc_file() {
    const fs::path p = fs::temp_directory_path() / "me-cache-test-abc.bin";
    std::ofstream(p, std::ios::binary) << "abc";
    return p;
}

/* Minimal timeline that just references the fixture URI — enough to push
 * entries through the engine's AssetHashCache via me_timeline_load_json. */
std::string timeline_json(const std::string& uri, const std::string& hex_hash) {
    std::string hash_field;
    if (!hex_hash.empty()) {
        hash_field = ",\"contentHash\":\"sha256:" + hex_hash + "\"";
    }
    return std::string(R"({
      "schemaVersion": 1,
      "frameRate":  {"num": 30, "den": 1},
      "resolution": {"width": 1280, "height": 720},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets":[{"id":"a1","kind":"video","uri":"file://)") +
      uri + R"("
          )" + hash_field + R"(
      }],
      "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
        {"type":"video","id":"c1","assetId":"a1",
         "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
         "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
      ]}]}],
      "output":{"compositionId":"main"}
    })";
}

}  // namespace

TEST_CASE("me_cache_stats returns zeros on a fresh engine") {
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    me_cache_stats_t stats{};
    CHECK(me_cache_stats(eng, &stats) == ME_OK);
    CHECK(stats.memory_bytes_used == 0);
    CHECK(stats.entry_count == 0);
    CHECK(stats.hit_count == 0);
    CHECK(stats.miss_count == 0);

    me_engine_destroy(eng);
}

TEST_CASE("me_cache_stats rejects NULL args") {
    me_cache_stats_t stats{};
    CHECK(me_cache_stats(nullptr, &stats) == ME_E_INVALID_ARG);
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);
    CHECK(me_cache_stats(eng, nullptr) == ME_E_INVALID_ARG);
    me_engine_destroy(eng);
}

TEST_CASE("seeded contentHash populates entry_count without recompute") {
    const fs::path f = write_abc_file();
    const std::string abc_hash =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    me_timeline_t* tl = nullptr;
    const std::string j = timeline_json(f.string(), abc_hash);
    REQUIRE(me_timeline_load_json(eng, j.data(), j.size(), &tl) == ME_OK);

    me_cache_stats_t stats{};
    CHECK(me_cache_stats(eng, &stats) == ME_OK);
    CHECK(stats.entry_count == 1);
    /* Seeded hashes bypass compute — no miss counted. */
    CHECK(stats.miss_count == 0);

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    fs::remove(f);
}

TEST_CASE("me_cache_clear drops AssetHashCache entries") {
    const fs::path f = write_abc_file();
    const std::string abc_hash =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    me_timeline_t* tl = nullptr;
    const std::string j = timeline_json(f.string(), abc_hash);
    REQUIRE(me_timeline_load_json(eng, j.data(), j.size(), &tl) == ME_OK);

    me_cache_stats_t before{};
    me_cache_stats(eng, &before);
    REQUIRE(before.entry_count == 1);

    CHECK(me_cache_clear(eng) == ME_OK);

    me_cache_stats_t after{};
    me_cache_stats(eng, &after);
    CHECK(after.entry_count == 0);

    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    fs::remove(f);
}

TEST_CASE("me_cache_invalidate_asset removes entries matching a content hash") {
    const fs::path f1 = write_abc_file();
    const fs::path f2 = fs::temp_directory_path() / "me-cache-test-other.bin";
    std::ofstream(f2, std::ios::binary) << "different";

    const std::string abc_hash =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::string diff_hash = std::string(64, 'd');  /* bogus hex, seed-only */

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    me_timeline_t* tl1 = nullptr;
    const std::string j1 = timeline_json(f1.string(), abc_hash);
    REQUIRE(me_timeline_load_json(eng, j1.data(), j1.size(), &tl1) == ME_OK);

    me_timeline_t* tl2 = nullptr;
    const std::string j2 = timeline_json(f2.string(), diff_hash);
    REQUIRE(me_timeline_load_json(eng, j2.data(), j2.size(), &tl2) == ME_OK);

    me_cache_stats_t before{};
    me_cache_stats(eng, &before);
    REQUIRE(before.entry_count == 2);

    /* Invalidate only the `abc` asset — `diff_hash` entry should survive. */
    CHECK(me_cache_invalidate_asset(eng, (std::string("sha256:") + abc_hash).c_str()) == ME_OK);

    me_cache_stats_t after{};
    me_cache_stats(eng, &after);
    CHECK(after.entry_count == 1);

    /* Bare hex form (no "sha256:" prefix) is also accepted. */
    CHECK(me_cache_invalidate_asset(eng, diff_hash.c_str()) == ME_OK);
    me_cache_stats(eng, &after);
    CHECK(after.entry_count == 0);

    me_timeline_destroy(tl2);
    me_timeline_destroy(tl1);
    me_engine_destroy(eng);
    fs::remove(f1);
    fs::remove(f2);
}
