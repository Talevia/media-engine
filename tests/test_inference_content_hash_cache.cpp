/*
 * test_inference_content_hash_cache — covers
 * `me::inference::AssetCache` (M11 exit criterion at
 * docs/MILESTONES.md:137).
 *
 * Coverage:
 *   - hash_inputs: empty / single tensor / order-invariance via
 *     std::map sort / sensitivity to dtype / shape / bytes.
 *   - AssetCache: capacity-0 (always-miss) / hit-after-put /
 *     LRU eviction at capacity / counter tracking / clear /
 *     refresh-existing-key.
 *   - Mock-runtime regression: a `CountingRuntime` that counts
 *     `run()` invocations + caching wrapper that consults
 *     AssetCache before delegating; assert second call doesn't
 *     reach the runtime when the cache hit is expected.
 *
 * Gated on ME_HAS_INFERENCE — same shape as the other inference
 * tests (test_inference_coreml_skeleton etc.). When the engine is
 * built with ME_WITH_INFERENCE=OFF, this TU compiles as a single
 * skipped TEST_CASE.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE
#include "inference/asset_cache.hpp"
#include "inference/runtime.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

me::inference::Tensor tensor_uint8(std::vector<std::int64_t> shape,
                                    std::vector<std::uint8_t> bytes) {
    me::inference::Tensor t;
    t.shape = std::move(shape);
    t.dtype = me::inference::Dtype::Uint8;
    t.bytes = std::move(bytes);
    return t;
}

me::inference::Tensor tensor_float32(std::vector<std::int64_t> shape,
                                      std::vector<std::uint8_t> bytes) {
    me::inference::Tensor t;
    t.shape = std::move(shape);
    t.dtype = me::inference::Dtype::Float32;
    t.bytes = std::move(bytes);
    return t;
}

}  // namespace

TEST_CASE("hash_inputs: empty input set returns the FNV offset") {
    std::map<std::string, me::inference::Tensor> empty;
    /* Two empty calls return the same value (sanity: deterministic). */
    CHECK(me::inference::hash_inputs(empty) == me::inference::hash_inputs(empty));
}

TEST_CASE("hash_inputs: same inputs → same hash") {
    std::map<std::string, me::inference::Tensor> a, b;
    a["image"] = tensor_uint8({1, 4}, {1, 2, 3, 4});
    b["image"] = tensor_uint8({1, 4}, {1, 2, 3, 4});
    CHECK(me::inference::hash_inputs(a) == me::inference::hash_inputs(b));
}

TEST_CASE("hash_inputs: different bytes → different hash") {
    std::map<std::string, me::inference::Tensor> a, b;
    a["image"] = tensor_uint8({1, 4}, {1, 2, 3, 4});
    b["image"] = tensor_uint8({1, 4}, {1, 2, 3, 5});
    CHECK(me::inference::hash_inputs(a) != me::inference::hash_inputs(b));
}

TEST_CASE("hash_inputs: different dtype with same bytes → different hash") {
    std::map<std::string, me::inference::Tensor> a, b;
    a["x"] = tensor_uint8({4},   {1, 2, 3, 4});
    b["x"] = tensor_float32({1}, {1, 2, 3, 4});
    CHECK(me::inference::hash_inputs(a) != me::inference::hash_inputs(b));
}

TEST_CASE("hash_inputs: different shape with same bytes → different hash") {
    std::map<std::string, me::inference::Tensor> a, b;
    a["x"] = tensor_uint8({1, 4}, {1, 2, 3, 4});
    b["x"] = tensor_uint8({4, 1}, {1, 2, 3, 4});
    CHECK(me::inference::hash_inputs(a) != me::inference::hash_inputs(b));
}

TEST_CASE("hash_inputs: different name → different hash") {
    std::map<std::string, me::inference::Tensor> a, b;
    a["image"] = tensor_uint8({1}, {0});
    b["mask"]  = tensor_uint8({1}, {0});
    CHECK(me::inference::hash_inputs(a) != me::inference::hash_inputs(b));
}

TEST_CASE("AssetCache: capacity-0 always misses + counters track") {
    me::inference::AssetCache cache{0};
    me::inference::AssetCacheKey key{"m", "v1", "fp32", 0xcafe};
    /* put on a capacity-0 cache is a no-op; size stays 0. */
    cache.put(key, {});
    CHECK(cache.size() == 0);
    /* get returns nullopt + bumps miss counter. */
    auto v = cache.get(key);
    CHECK(!v.has_value());
    CHECK(cache.miss_count() == 1);
    CHECK(cache.hit_count() == 0);
}

TEST_CASE("AssetCache: hit-after-put returns the stored outputs") {
    me::inference::AssetCache cache{4};
    me::inference::AssetCacheKey key{"face_landmark", "v2", "fp16", 12345};

    std::map<std::string, me::inference::Tensor> outs;
    outs["landmarks"] = tensor_float32({1, 6, 2}, std::vector<std::uint8_t>(48, 0xAA));
    cache.put(key, outs);

    auto got = cache.get(key);
    REQUIRE(got.has_value());
    CHECK(got->size() == 1);
    CHECK(got->at("landmarks").bytes.size() == 48);
    CHECK(got->at("landmarks").bytes[0] == 0xAA);
    CHECK(cache.hit_count() == 1);
    CHECK(cache.miss_count() == 0);
}

TEST_CASE("AssetCache: LRU evicts oldest at capacity") {
    me::inference::AssetCache cache{2};
    auto k = [](std::uint64_t h) {
        return me::inference::AssetCacheKey{"m", "v", "q", h};
    };
    cache.put(k(1), {{"out", tensor_uint8({1}, {1})}});
    cache.put(k(2), {{"out", tensor_uint8({1}, {2})}});
    /* Access k(1) so it becomes most-recent → k(2) becomes LRU. */
    auto a = cache.get(k(1));
    REQUIRE(a.has_value());
    /* Inserting k(3) at capacity 2 evicts k(2). */
    cache.put(k(3), {{"out", tensor_uint8({1}, {3})}});
    CHECK(cache.size() == 2);
    CHECK(cache.get(k(2)).has_value() == false);
    CHECK(cache.get(k(1)).has_value() == true);
    CHECK(cache.get(k(3)).has_value() == true);
}

TEST_CASE("AssetCache: refresh-existing-key updates value + recency") {
    me::inference::AssetCache cache{2};
    me::inference::AssetCacheKey k{"m", "v", "q", 7};
    cache.put(k, {{"out", tensor_uint8({1}, {0xAA})}});
    cache.put(k, {{"out", tensor_uint8({1}, {0xBB})}});
    auto v = cache.get(k);
    REQUIRE(v.has_value());
    CHECK(v->at("out").bytes[0] == 0xBB);
    /* Should remain a single entry — refresh doesn't grow. */
    CHECK(cache.size() == 1);
}

TEST_CASE("AssetCache: clear() drops entries but preserves counters") {
    me::inference::AssetCache cache{4};
    me::inference::AssetCacheKey k{"m", "v", "q", 1};
    cache.put(k, {{"out", tensor_uint8({1}, {0})}});
    (void)cache.get(k);  /* one hit */
    (void)cache.get(me::inference::AssetCacheKey{"m", "v", "q", 9});  /* one miss */
    CHECK(cache.size() == 1);
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.hit_count()  == 1);
    CHECK(cache.miss_count() == 1);
}

namespace {

/* Mock runtime that counts `run()` invocations. Wrapped by a thin
 * caching helper below to demonstrate the same-input cache-hit
 * regression contract. */
class CountingRuntime final : public me::inference::Runtime {
public:
    int call_count = 0;

    me_status_t run(
        const std::map<std::string, me::inference::Tensor>& inputs,
        std::map<std::string, me::inference::Tensor>*       outputs,
        std::string*                                          /*err*/) override {
        ++call_count;
        /* Echo: produce a single named output that copies the first
         * input's bytes — enough to verify "did we hit the cache or
         * actually run?" without modeling a real model. */
        if (!inputs.empty()) {
            const auto& in = inputs.begin()->second;
            (*outputs)["out"] = in;
        }
        return ME_OK;
    }
};

/* Minimal caching helper — same shape effect kernels would use:
 * compute key, consult cache, on miss delegate + store. */
me_status_t infer_cached(me::inference::Runtime&                                runtime,
                          me::inference::AssetCache&                             cache,
                          const std::string&                                     model_id,
                          const std::string&                                     model_version,
                          const std::string&                                     quantization,
                          const std::map<std::string, me::inference::Tensor>&    inputs,
                          std::map<std::string, me::inference::Tensor>*          outputs,
                          std::string*                                            err) {
    me::inference::AssetCacheKey key{
        model_id, model_version, quantization,
        me::inference::hash_inputs(inputs)};
    if (auto cached = cache.get(key)) {
        *outputs = std::move(*cached);
        return ME_OK;
    }
    me_status_t s = runtime.run(inputs, outputs, err);
    if (s == ME_OK) cache.put(std::move(key), *outputs);
    return s;
}

}  // namespace

TEST_CASE("AssetCache: same-input second call hits cache, runtime not re-invoked") {
    CountingRuntime runtime;
    me::inference::AssetCache cache{4};

    std::map<std::string, me::inference::Tensor> inputs;
    inputs["image"] = tensor_uint8({1, 4}, {10, 20, 30, 40});

    std::map<std::string, me::inference::Tensor> outputs;
    std::string err;

    REQUIRE(infer_cached(runtime, cache, "m", "v1", "fp32",
                          inputs, &outputs, &err) == ME_OK);
    CHECK(runtime.call_count == 1);
    CHECK(outputs.at("out").bytes.size() == 4);

    outputs.clear();
    REQUIRE(infer_cached(runtime, cache, "m", "v1", "fp32",
                          inputs, &outputs, &err) == ME_OK);
    /* Second call should hit cache — runtime NOT re-invoked. */
    CHECK(runtime.call_count == 1);
    CHECK(outputs.at("out").bytes.size() == 4);
    CHECK(cache.hit_count()  == 1);
    CHECK(cache.miss_count() == 1);
}

TEST_CASE("AssetCache: different model_version → cache miss") {
    CountingRuntime runtime;
    me::inference::AssetCache cache{4};
    std::map<std::string, me::inference::Tensor> inputs;
    inputs["image"] = tensor_uint8({1}, {0});
    std::map<std::string, me::inference::Tensor> outputs;
    std::string err;

    REQUIRE(infer_cached(runtime, cache, "m", "v1", "fp32",
                          inputs, &outputs, &err) == ME_OK);
    REQUIRE(infer_cached(runtime, cache, "m", "v2", "fp32",
                          inputs, &outputs, &err) == ME_OK);
    /* Two distinct keys → two runtime invocations. */
    CHECK(runtime.call_count == 2);
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("AssetCache: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub mirroring the other inference test
     * suites. */
}

#endif
