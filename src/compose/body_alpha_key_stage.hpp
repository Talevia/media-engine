/* `me::compose::register_body_alpha_key_kind` — register the
 * `TaskKindId::RenderBodyAlphaKey` kernel with the global task
 * registry.
 *
 * M11 `body-alpha-key-compose-graph-stage-impl`. Sibling of
 * `face_sticker_stage` / `face_mosaic_stage` — same per-effect
 * TaskKindId pattern. Differs in the upstream resolver:
 * `mask_resolver` returns a per-frame 8-bit alpha plane (sized
 * to the frame) rather than a list of bboxes.
 *
 * Inputs:
 *   - one RGBA8 frame (the upstream layer's output).
 *
 * Properties:
 *   - `mask_asset_uri` (string — pre-resolved by compose_compile
 *     from `maskAssetId` via the timeline's assets map).
 *   - `frame_t_num` / `frame_t_den` (rational — drives the mask
 *     resolver's closest-frame selection).
 *   - `feather_radius_px` (int64 — must be ≥ 0; default 0).
 *   - `invert` (int64 — 0 = use mask as-is, 1 = invert per
 *     `BodyAlphaKeyEffectParams::invert`).
 *
 * Outputs:
 *   - one RGBA8 frame with the alpha channel multiplied by the
 *     resolved (optionally inverted, optionally feathered) mask.
 *
 * The kernel calls `me::compose::resolve_mask_alpha_from_file`
 * for the per-frame alpha plane (file-mode resolver — JSON
 * sidecar with base64-encoded alpha bytes). The runtime variant
 * (running portrait segmentation through `me::inference::Runtime`)
 * is a future cycle's wiring; this stage's `mask_asset_uri` is
 * expected to point at a pre-computed file fixture for now.
 *
 * Empty mask (resolver returns 0×0) → kernel no-op (frame
 * passes through unchanged). This matches mask_resolver's
 * documented "no mask available for this frame" representation.
 *
 * `time_invariant=false` because the resolver samples per
 * `frame_t`.
 */
#pragma once

namespace me::compose {

void register_body_alpha_key_kind();

}  // namespace me::compose
