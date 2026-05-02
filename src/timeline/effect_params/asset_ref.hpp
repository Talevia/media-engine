/* ML-asset reference types for effect param schemas (M11
 * `effect-kind-ml-asset-input-schema`).
 *
 * Three sibling typed structs that effect param structs include
 * when the effect consumes an ML-produced asset (Landmark, Mask,
 * Keypoints) from the timeline's `assets` array. The bullet
 * (BACKLOG, M11 exit criterion at `docs/MILESTONES.md:140`)
 * called for "typed schema variants" alongside the existing
 * primitive types — these structs are the M11-shaped equivalents:
 * each carries the asset id (referring to a `Timeline::assets`
 * entry) plus an optional frame-time selection used when the
 * effect needs a sample older / newer than the current compose
 * frame (e.g. trailing-stabilizer effects that look ahead).
 *
 * Backward compatibility. Pre-cycle JSON used a flat string field
 * (`"landmarkAssetId": "id"`); the loader still accepts that
 * shape (the asset_id field is populated, has_time_offset stays
 * false). New JSON authoring tools target the typed object form
 * (`"landmarkAssetRef": {"assetId": "id", "timeOffset": {...}}`)
 * for explicit time-offset control. JSON schema docs land in
 * `docs/TIMELINE_SCHEMA.md` alongside the existing
 * `lutPath` / `landmarkAssetId` fields.
 *
 * Unconsumed for now: `KeypointAssetRef` is defined but no effect
 * params struct holds one; skeleton-based effects (pose-tracking
 * stickers etc.) are M11+ work — the type is forward-declared so
 * the schema is symmetrical and the JSON parser surface is in
 * place when those effects land.
 *
 * Determinism. Asset-ref structs are pure data; no behavior tied
 * to construction order. The `time_offset` field is rational
 * (`me_rational_t`) per the engine's "no float for time" rule
 * (CLAUDE.md architecture invariant 3 / VISION §3.5).
 */
#pragma once

#include "media_engine/types.h"

#include <string>

namespace me {

/* Reference to a Landmark asset (Asset.kind == AssetKind::Landmark)
 * consumed by `face_sticker` / `face_mosaic` effects. */
struct LandmarkAssetRef {
    /* Asset.id from the timeline's `assets` array. Compose-time
     * consumer resolves + rejects on miss. Empty string means
     * "no reference" (effect won't attempt landmark resolution). */
    std::string   asset_id;

    /* Optional frame-time offset (added to the compose frame's
     * timestamp when sampling the landmark stream). Use case:
     * an effect that wants the next frame's landmark for motion
     * prediction. `has_time_offset == false` ⇒ sample at compose
     * frame's exact timestamp. */
    me_rational_t time_offset      { 0, 1 };
    bool          has_time_offset  = false;
};

/* Reference to a Mask asset (Asset.kind == AssetKind::Mask)
 * consumed by `body_alpha_key` effects. Same shape as
 * `LandmarkAssetRef` — the type distinction is for compile-time
 * safety so an effect declaring it consumes a mask can't be
 * handed a landmark stream. */
struct MaskAssetRef {
    std::string   asset_id;
    me_rational_t time_offset      { 0, 1 };
    bool          has_time_offset  = false;
};

/* Reference to a Keypoint asset (Asset.kind == AssetKind::Keypoint
 * — pose / skeleton with connectivity per M11 schema). No effect
 * consumes this today; the type is registered ahead of skeleton-
 * based effects so the JSON parser shape is symmetrical with
 * landmark / mask refs. */
struct KeypointAssetRef {
    std::string   asset_id;
    me_rational_t time_offset      { 0, 1 };
    bool          has_time_offset  = false;
};

}  // namespace me
