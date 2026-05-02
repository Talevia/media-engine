/* `EffectKind::FaceMosaic` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 6.
 *
 * M11 face-mosaic effect — deferred impl. Privacy-focused
 * counterpart to face_sticker: applies a per-block mosaic
 * (pixelation or blur) to the landmark's bounding-box region.
 * Common use: anonymise faces in user-uploaded video.
 *
 * Same registered-but-deferred shape as face_sticker (cycle 30).
 * Tracked under `face-mosaic-impl` in the BACKLOG. The compose
 * impl will resolve the landmark stream → bbox per frame →
 * apply pixelate/blur within the bbox; the algorithms themselves
 * are deterministic byte-math (mean over block × replicate, or
 * box-filter blur) so VISION §3.1 byte-identity holds. */
#pragma once

#include "timeline/effect_params/asset_ref.hpp"

#include <cstdint>
#include <string>

namespace me {

struct FaceMosaicEffectParams {
    enum class Kind : uint8_t {
        Pixelate = 0,   /* mean over `block_size_px²` × replicate (mosaic look). */
        Blur     = 1,   /* box filter at radius `block_size_px / 2`. */
    };

    /* References an Asset.id with kind == AssetKind::Landmark, plus
     * an optional frame-time offset (cycle 31
     * `effect-kind-ml-asset-input-schema`). Loader accepts the
     * legacy `"landmarkAssetId": "<id>"` and the typed
     * `"landmarkAssetRef": {...}` shapes. Compose-time consumer
     * resolves + rejects on miss. */
    LandmarkAssetRef landmark;

    /* Block size in pixels. Both algorithms use this as the unit
     * size — Pixelate = mean of `block_size_px × block_size_px`
     * tile replicated within the tile; Blur = effective radius
     * `block_size_px / 2`. Default 16 = noticeable mosaic at
     * face-sized bboxes (~200 px wide → 12 blocks across the
     * face) without over-blurring at small bboxes. */
    int  block_size_px = 16;

    Kind kind          = Kind::Pixelate;
};

}  // namespace me
