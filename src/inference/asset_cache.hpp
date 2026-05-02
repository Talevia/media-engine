/* `me::inference::AssetCache` — in-process LRU cache for inference
 * outputs keyed on `(model_id, model_version, quantization,
 * input_hash)`.
 *
 * M11 exit criterion at `docs/MILESTONES.md:137` requires inference
 * assets to flow through the §3.3 contentHash cache so that
 * scrubbing / re-export / repeat-frame paths don't re-run the
 * model. The existing `me::sched::OutputCache`
 * (`src/scheduler/output_cache.hpp`) is purpose-built for graph
 * `OutputSlot::v` variants — extending it to carry inference
 * `Tensor` outputs would mean leaking the inference dtype enum into
 * the graph variant. AssetCache stays sibling-shaped: same LRU +
 * counter contract, different value type.
 *
 * Determinism caveat. Inference outputs themselves are non-
 * deterministic per VISION §3.4 (CoreML / ONNX-CPU may produce
 * bit-different bytes on the same input). The cache reuses
 * whatever the runtime returned the first time — so a cache hit
 * delivers the same bytes as the first miss for that key, not the
 * bytes a fresh `Runtime::run()` would produce on a cold engine.
 * That's the point: cache hit means "you've seen this combination
 * already; here's the answer".
 *
 * Threading. Public methods take `mu_` so concurrent get/put from
 * different effect-kernel evaluations is safe. Iteration order is
 * not exposed.
 *
 * Persistence. In-process only. Persistent disk-backed inference
 * cache is the `inference-asset-disk-cache` follow-up — that
 * lands once we confirm the in-process cache pays off on actual
 * scrub / re-export workloads.
 */
#pragma once

#include "inference/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace me::inference {

/* Cache key: 4-tuple per the M11 criterion. The three model-
 * identity strings are owned (std::string, not std::string_view) so
 * the key survives caller mutations of any source buffer. */
struct AssetCacheKey {
    std::string   model_id;
    std::string   model_version;
    std::string   quantization;
    std::uint64_t input_hash = 0;

    bool operator==(const AssetCacheKey& o) const noexcept {
        return input_hash    == o.input_hash
            && model_id      == o.model_id
            && model_version == o.model_version
            && quantization  == o.quantization;
    }
};

struct AssetCacheKeyHash {
    std::size_t operator()(const AssetCacheKey& k) const noexcept;
};

/* Hash a `std::map<std::string, Tensor>` into a stable 64-bit key
 * suitable for `AssetCacheKey::input_hash`. Walks inputs in sorted
 * order (std::map iteration is sorted), mixes name + dtype + shape +
 * bytes via FNV-1a. The result is stable per process; not a
 * cryptographic hash. */
std::uint64_t hash_inputs(const std::map<std::string, Tensor>& inputs) noexcept;

/* Forward decl so this header doesn't pull in `core/engine_impl.hpp`
 * (which is private-only and would propagate transitively to every
 * effect TU that needs the cache key types). */

class AssetCache {
public:
    /* `capacity` = max number of (key, outputs) entries; 0 disables
     * the cache entirely (every get misses, put is a no-op). Same
     * shape as `me::sched::OutputCache` for consistency. */
    explicit AssetCache(std::size_t capacity = 0);

    AssetCache(const AssetCache&)            = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    /* Returns a copy of the cached outputs on hit; nullopt on miss
     * or when capacity is 0. Touches LRU recency on hit. The copy
     * is intentional — outputs are owned by the cache, callers
     * mutate freely. */
    std::optional<std::map<std::string, Tensor>> get(const AssetCacheKey& key);

    /* Inserts or refreshes an entry. Evicts the oldest entry if
     * insertion would exceed capacity. No-op when capacity is 0.
     * Outputs are moved-from when the cache stores them. */
    void put(AssetCacheKey key, std::map<std::string, Tensor> outputs);

    std::size_t  size()       const noexcept;
    std::int64_t hit_count()  const noexcept;
    std::int64_t miss_count() const noexcept;

    void clear();

private:
    struct Entry {
        AssetCacheKey                  key;
        std::map<std::string, Tensor>  value;
    };

    mutable std::mutex                                                            mu_;
    std::list<Entry>                                                              lru_;
    std::unordered_map<AssetCacheKey, std::list<Entry>::iterator, AssetCacheKeyHash> index_;
    std::size_t                                                                   capacity_;
    std::int64_t                                                                  hits_   = 0;
    std::int64_t                                                                  misses_ = 0;
};

}  // namespace me::inference

struct me_engine;

namespace me::inference {

/* `run_cached` — production-grade wrapper that consults the engine's
 * shared `AssetCache` before delegating to `Runtime::run` and stores
 * the runtime's outputs on miss. Same shape as the test-local helper
 * in `tests/test_inference_content_hash_cache.cpp`, but keyed off the
 * engine's process-wide cache (`me_engine::asset_cache`) so all
 * effect stages share one LRU.
 *
 * M11 exit criterion at `docs/MILESTONES.md:137` requires inference
 * assets to flow through the §3.3 contentHash cache; this is the
 * single public entry point that effect kernels call to satisfy
 * that — `Runtime::run` directly bypasses the cache and is reserved
 * for unit-test scaffolding.
 *
 * Behavior:
 *   - `engine` MUST be non-NULL and inference-built (`ME_HAS_INFERENCE`).
 *     A NULL engine returns `ME_E_INVALID_ARG`.
 *   - On hit: `*outputs` is overwritten with a copy of the cached
 *     map; `Runtime::run` is NOT invoked. Returns `ME_OK`.
 *   - On miss: `Runtime::run` is invoked; on `ME_OK` the result is
 *     stored under the computed key and forwarded to `*outputs`.
 *     Non-OK runtime results bypass the cache entirely (errors are
 *     never stored).
 *
 * The cache key is derived per the M11 contract:
 * `(model_id, model_version, quantization, hash_inputs(inputs))`. */
me_status_t run_cached(me_engine*                                       engine,
                       Runtime&                                          runtime,
                       const std::string&                                model_id,
                       const std::string&                                model_version,
                       const std::string&                                quantization,
                       const std::map<std::string, Tensor>&              inputs,
                       std::map<std::string, Tensor>*                    outputs,
                       std::string*                                      err);

}  // namespace me::inference
