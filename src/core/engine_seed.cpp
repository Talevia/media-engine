#include "core/engine_seed.hpp"

#include "resource/asset_hash_cache.hpp"

namespace me::detail {

void seed_engine_from_timeline(me_engine& engine, const me::Timeline& tl) {
    /* Asset-hash seed: pre-warm the cache with each Asset's declared
     * sha256. Iterating Timeline::assets (unordered_map) is safe —
     * AssetHashCache::seed is idempotent per URI so iteration order
     * can't affect observable state (VISION §3.1 determinism).
     * Assets that omit contentHash leave the cache cold; downstream
     * `get_or_compute` stream-hashes on first demand. */
    if (engine.asset_hashes) {
        for (const auto& [id, asset] : tl.assets) {
            if (!asset.content_hash.empty()) {
                engine.asset_hashes->seed(asset.uri, asset.content_hash);
            }
        }
    }

    /* Future seed consumers extend here:
     *
     *   - M2 color pipeline (`me::color::make_pipeline`) may preheat an
     *     OCIO Processor cache keyed by asset.color_space + timeline
     *     working-space.
     *   - M3 effect-LUT preheat when a timeline's clip.effects[] set
     *     references LUTs that want to be mmap'd before the first
     *     render starts.
     *
     * The second consumer is the trigger-point for reconsidering the
     * loader signature (option (a) in the original bullet). As long as
     * the seed needs nothing from the loader beyond the produced
     * Timeline IR, this free-function extension point is sufficient. */
}

}  // namespace me::detail
