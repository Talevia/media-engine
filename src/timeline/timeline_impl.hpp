/* Internal timeline IR. Minimal — expands as schema v1 coverage grows.
 *
 * Phase-1 scope: single video track with N contiguous clips, no effects,
 * no transforms. Everything else is rejected at load time with
 * ME_E_UNSUPPORTED.
 *
 * Assets are first-class: `Timeline::assets` maps id → Asset, and clips
 * reference an asset by `asset_id` rather than duplicating the URI. This
 * keeps per-asset metadata (URI, content hash, and future colorSpace /
 * metadata bag) orthogonal to per-clip metadata (time window) — same
 * asset referenced by N clips stores only once.
 */
#pragma once

#include "media_engine/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace me {

struct Asset {
    std::string uri;          /* resolved later (strip file:// at I/O time) */

    /* Optional: lowercase hex sha256 digest (64 chars) of the asset bytes.
     * Sourced from JSON `contentHash` when present; empty means unknown
     * and the engine's AssetHashCache computes lazily on first need. */
    std::string content_hash;
};

struct Clip {
    /* Reference into Timeline::assets. Guaranteed non-empty after load;
     * loader rejects clips with unknown asset_id. */
    std::string   asset_id;

    me_rational_t time_start   { 0, 1 };
    me_rational_t time_duration{ 0, 1 };
    me_rational_t source_start { 0, 1 };
};

struct Timeline {
    me_rational_t frame_rate   { 30, 1 };
    me_rational_t duration     { 0, 1 };
    int           width        { 0 };
    int           height       { 0 };

    /* id → Asset. Lookup-only (never iterated), so std::unordered_map is
     * safe for determinism — iteration order would be undefined, but we
     * resolve by key. */
    std::unordered_map<std::string, Asset> assets;

    std::vector<Clip> clips;
};

}  // namespace me

struct me_timeline {
    me::Timeline tl;
};
