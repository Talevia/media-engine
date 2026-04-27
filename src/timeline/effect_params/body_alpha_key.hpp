/* `EffectKind::BodyAlphaKey` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 7.
 *
 * M11 body-alpha-key effect — deferred impl. Applies a portrait
 * segmentation mask (Asset.kind == AssetKind::Mask) as the
 * foreground alpha channel. Use case: green-screen-without-
 * greenscreen — drop the camera's natural background, composite
 * the subject onto a different layer.
 *
 * Different from face_sticker / face_mosaic: consumes a
 * `mask_asset_id` (AssetKind::Mask, cycle 28's IR work)
 * referencing a per-frame alpha sequence rather than landmarks.
 * Mask interpretation: 8-bit alpha per pixel; 0 = background
 * (output transparent), 255 = foreground (output opaque).
 *
 * Same registered-but-deferred shape as face_sticker / face_mosaic.
 * Tracked under `body-alpha-key-impl` in the BACKLOG. The compose
 * impl will: read mask[frame_t] → optionally invert → optionally
 * feather (box-blur the alpha edge by `feather_radius_px`) → write
 * mask × input.alpha into output.alpha. The byte-math is
 * deterministic; VISION §3.1 byte-identity holds. */
#pragma once

#include <string>

namespace me {

struct BodyAlphaKeyEffectParams {
    /* References an Asset.id with kind == AssetKind::Mask.
     * Compose-time consumer resolves + rejects on miss. */
    std::string mask_asset_id;

    /* Soften the mask edge by box-blurring the alpha for this
     * many pixels of radius. 0 = sharp edge (no feathering);
     * common values 1..16 for screen-realistic anti-alias.
     * Larger values smear the edge significantly. */
    int  feather_radius_px = 0;

    /* When true, invert the mask before applying — useful when
     * the upstream segmentation labels foreground=0 instead of
     * the convention here. */
    bool invert            = false;
};

}  // namespace me
