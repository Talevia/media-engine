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

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

/* Per-asset color space override (TIMELINE_SCHEMA.md §Color). Each axis is
 * independently optional — `Unspecified` means "trust container metadata
 * for this axis". Populated into Asset only when JSON explicitly carries
 * `"colorSpace":{...}`; a missing object leaves Asset::color_space as
 * std::nullopt (= trust container for everything).
 *
 * M2 OCIO pipeline consumes these enums to pick a working-space transform.
 * Keep the enumeration stable with the schema string tables in the loader;
 * adding a value needs a matching entry in both directions. */
struct ColorSpace {
    enum class Primaries : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020, P3_D65
    };
    enum class Transfer : uint8_t {
        Unspecified = 0, BT709, SRGB, Linear, PQ, HLG, Gamma22, Gamma28
    };
    enum class Matrix : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020NC, Identity
    };
    enum class Range : uint8_t {
        Unspecified = 0, Limited, Full
    };

    Primaries primaries{Primaries::Unspecified};
    Transfer  transfer {Transfer::Unspecified};
    Matrix    matrix   {Matrix::Unspecified};
    Range     range    {Range::Unspecified};
};

struct Asset {
    std::string uri;          /* resolved later (strip file:// at I/O time) */

    /* Optional: lowercase hex sha256 digest (64 chars) of the asset bytes.
     * Sourced from JSON `contentHash` when present; empty means unknown
     * and the engine's AssetHashCache computes lazily on first need. */
    std::string content_hash;

    /* Optional override of container color-space metadata. Present iff
     * JSON asset object carried `"colorSpace":{...}`. M2-prep — M2 OCIO
     * is the only consumer. */
    std::optional<ColorSpace> color_space;
};

/* 2D transform applied when the clip composites onto the output canvas.
 * Phase-1 is static only: every field is a plain double, populated from
 * the `{"static": <number>}` form in JSON. The animated
 * (`{"keyframes": [...]}`) form is deliberately rejected by the loader
 * with ME_E_UNSUPPORTED so that this struct stays a stable shape while
 * animated-parameter support lands with M3. Defaults are identity so
 * `Transform{}` is a valid "no-op" state for code paths that read the
 * transform unconditionally.
 *
 * Stored as std::optional<Transform> on Clip: nullopt = "the JSON clip
 * has no `transform` key at all" (different from Transform{} which means
 * "transform key present but all fields defaulted to identity"). This
 * distinction lets downstream code optionally fast-path clips that truly
 * omit transforms. */
struct Transform {
    double translate_x  = 0.0;
    double translate_y  = 0.0;
    double scale_x      = 1.0;
    double scale_y      = 1.0;
    double rotation_deg = 0.0;
    double opacity      = 1.0;
    double anchor_x     = 0.5;
    double anchor_y     = 0.5;
};

struct Clip {
    /* Reference into Timeline::assets. Guaranteed non-empty after load;
     * loader rejects clips with unknown asset_id. */
    std::string   asset_id;

    me_rational_t time_start   { 0, 1 };
    me_rational_t time_duration{ 0, 1 };
    me_rational_t source_start { 0, 1 };

    /* Optional 2D transform. Populated iff JSON clip carries a
     * `transform` object (empty object = Transform{} with identity
     * defaults; absent = nullopt). Phase-1 is static-only; the loader
     * rejects the `keyframes` animated form. */
    std::optional<Transform> transform;
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
