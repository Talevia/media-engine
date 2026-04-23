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

TEST_CASE("AssetHashCache::seed for the same URI with a different hash is last-wins") {
    /* Policy: explicit `seed(uri, hash)` overwrites any prior hash for
     * the same URI (asset_hash_cache.cpp: `map_[uri] = hash` —
     * operator[] assigns unconditionally). This is deliberately
     * asymmetric with `get_or_compute`'s `try_emplace` (first-wins) —
     * the explicit seed path is authoritative because callers driving
     * it have fresher information than the hash-on-first-read path.
     *
     * Pinned here so a future refactor switching to `try_emplace` for
     * seed doesn't silently change the contract; a caller updating
     * asset.contentHash via a fresh me_timeline_load_json would
     * otherwise silently see the stale hash. */
    AssetHashCache cache;
    const std::string uri = "file:///nonexistent/asset.mp4";
    const std::string hash_a = std::string(64, 'a');
    const std::string hash_b = std::string(64, 'b');

    cache.seed(uri, hash_a);
    REQUIRE(cache.peek(uri) == hash_a);

    cache.seed(uri, hash_b);
    CHECK(cache.peek(uri) == hash_b);   /* last-wins */
    CHECK(cache.size() == 1);           /* still one entry, not two */

    /* Empty subsequent seed is a documented no-op — prior hash survives. */
    cache.seed(uri, "");
    CHECK(cache.peek(uri) == hash_b);
}

TEST_CASE("AssetHashCache::seed on different URIs accumulates independently") {
    /* Corollary: two distinct URIs pointing to the same hash value is
     * still two independent entries. Protects against a hypothetical
     * "dedupe by hash" optimisation that would break the URI-keyed
     * eviction contract (invalidate_by_hash removes entries matching
     * a hash, but the forward map is URI-keyed). */
    AssetHashCache cache;
    const std::string uri_a = "file:///a.mp4";
    const std::string uri_b = "file:///b.mp4";
    const std::string shared_hash = std::string(64, 'c');

    cache.seed(uri_a, shared_hash);
    cache.seed(uri_b, shared_hash);

    CHECK(cache.peek(uri_a) == shared_hash);
    CHECK(cache.peek(uri_b) == shared_hash);
    CHECK(cache.size() == 2);
}
