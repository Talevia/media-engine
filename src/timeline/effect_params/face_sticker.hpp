/* `EffectKind::FaceSticker` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 5.
 *
 * M11 face-sticker effect — deferred impl. Consumes a Landmark asset
 * (Asset.kind == AssetKind::Landmark from cycle 28's IR work) by
 * `landmark_asset_id` reference plus a sticker image URI; positions
 * the sticker at face landmarks with optional scale/offset.
 *
 * Why deferred. The actual kernel needs an inference runtime that
 * consumes the landmark asset, plus a sticker-decoder kernel + a
 * compose-graph stage that overlays the sticker onto the RGBA8
 * frame. None of those exist pre-cycle 30. The registration here
 * lands the IR + JSON shape so timeline JSON authoring tools can
 * target `kind: "face_sticker"` ahead of the impl — same
 * registered-but-deferred pattern as `inverse_tonemap`'s pre-cycle
 * 24 state. The deferred impl is tracked as `face-sticker-impl` in
 * the P2 backlog (cycle 30 append).
 *
 * Defaults are "no-op"-ish so a misconfigured spec doesn't surprise
 * consumers: scale 1.0 means "use sticker's native size", offset 0
 * means "anchor at landmark centroid". When the impl lands, it
 * needs to define landmark-anchor-point semantics — typical
 * choices are landmark-bbox-centre (face_sticker style) or a
 * specific landmark index (e.g. nose-tip for nose-glasses
 * alignment). */
#pragma once

#include <string>

namespace me {

struct FaceStickerEffectParams {
    /* References an Asset.id with kind == AssetKind::Landmark.
     * Loader does not validate the cross-reference at parse time —
     * compose-time consumer resolves + rejects on miss. */
    std::string landmark_asset_id;

    /* Sticker image URI (PNG / WebP with alpha). Loaded lazily by
     * the impl; format detection via libavformat's probe at first
     * use. */
    std::string sticker_uri;

    /* Scale factor relative to the landmark bounding box. 1.0 =
     * sticker fits the bbox; > 1 oversize, < 1 undersize. Independent
     * X/Y so non-uniform stretching is possible (rarely useful but
     * canonical). */
    double      scale_x = 1.0;
    double      scale_y = 1.0;

    /* Pixel offset from landmark anchor (typically bbox centre).
     * Positive X = rightward, positive Y = downward (image-space). */
    double      offset_x = 0.0;
    double      offset_y = 0.0;
};

}  // namespace me
