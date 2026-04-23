#include <doctest/doctest.h>

#include "resource/asset_hash_cache.hpp"
#include "resource/content_hash.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace me::resource;

namespace {
/* RFC-style known vectors for SHA-256. */
const std::string kEmptyHash =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const std::string kAbcHash =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}  // namespace

TEST_CASE("sha256_hex matches the NIST 'abc' test vector") {
    const uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(sha256_hex(abc, sizeof(abc)) == kAbcHash);
}

TEST_CASE("sha256_hex on empty input matches the empty-string vector") {
    CHECK(sha256_hex(nullptr, 0) == kEmptyHash);
    CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(""), 0) == kEmptyHash);
}

TEST_CASE("sha256_hex_streaming matches direct hash on a temp file") {
    auto tmp = fs::temp_directory_path() / "me-test-content-hash.bin";
    const std::string payload = "the quick brown fox jumps over the lazy dog";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fwrite(payload.data(), 1, payload.size(), f);
        std::fclose(f);
    }
    std::string err;
    const std::string via_file = sha256_hex_streaming(tmp.string(), &err);
    const std::string via_bytes = sha256_hex(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    CHECK(err.empty());
    CHECK(via_file == via_bytes);
    fs::remove(tmp);
}

TEST_CASE("sha256_hex_streaming handles file:// URIs") {
    auto tmp = fs::temp_directory_path() / "me-test-content-hash-uri.bin";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        REQUIRE(f != nullptr);
        const char* s = "abc";
        std::fwrite(s, 1, 3, f);
        std::fclose(f);
    }
    const std::string uri = "file://" + tmp.string();
    CHECK(sha256_hex_streaming(uri) == kAbcHash);
    fs::remove(tmp);
}

TEST_CASE("sha256_hex_streaming on missing file reports an error") {
    std::string err;
    const std::string result =
        sha256_hex_streaming("/tmp/this-file-should-not-exist-xxx", &err);
    CHECK(result.empty());
    CHECK(!err.empty());
}

TEST_CASE("AssetHashCache::get_or_compute caches the first result") {
    AssetHashCache cache;
    auto tmp = fs::temp_directory_path() / "me-test-asset-cache.bin";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fwrite("abc", 1, 3, f);
        std::fclose(f);
    }
    CHECK(cache.size() == 0);
    const std::string first = cache.get_or_compute(tmp.string());
    CHECK(first == kAbcHash);
    CHECK(cache.size() == 1);

    /* Delete the file; second call must still hit the cache. */
    fs::remove(tmp);
    const std::string second = cache.get_or_compute(tmp.string());
    CHECK(second == kAbcHash);
    CHECK(cache.peek(tmp.string()) == kAbcHash);
}

TEST_CASE("AssetHashCache::seed skips recomputation") {
    AssetHashCache cache;
    const std::string uri = "file:///nonexistent/path.mp4";
    const std::string trusted = std::string(64, 'd');  /* any 64-char hex-shaped */
    cache.seed(uri, trusted);
    /* Even though the file doesn't exist, peek/get should return the seed. */
    CHECK(cache.peek(uri) == trusted);
    CHECK(cache.get_or_compute(uri) == trusted);
}

TEST_CASE("AssetHashCache::seed ignores empty hashes") {
    AssetHashCache cache;
    cache.seed("file:///anything.mp4", "");
    CHECK(cache.size() == 0);
}
