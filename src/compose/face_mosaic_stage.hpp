/* `me::compose::register_face_mosaic_kind` — register the
 * `TaskKindId::RenderFaceMosaic` kernel with the global task
 * registry.
 *
 * M11 `face-mosaic-compose-graph-stage-impl`. Sibling of
 * `face_sticker_stage` — same per-effect TaskKindId pattern (each
 * effect kind owns its own kind id; effects are registered as free
 * functions, never attached to Node instances per
 * `docs/ARCHITECTURE_GRAPH.md`).
 *
 * Inputs:
 *   - one RGBA8 frame (the upstream layer's output).
 *
 * Properties:
 *   - `landmark_asset_uri` (string — pre-resolved by compose_compile
 *     from `landmarkAssetId` via the timeline's assets map).
 *   - `frame_t_num` / `frame_t_den` (rational — drives the landmark
 *     resolver's closest-frame selection).
 *   - `block_size_px` (int64 — must be > 0; default 16).
 *   - `mosaic_kind` (int64 — 0 = Pixelate, 1 = Blur; matches
 *     `me::FaceMosaicEffectParams::Kind`).
 *
 * Outputs:
 *   - one RGBA8 frame with the mosaic applied within each
 *     resolved landmark bbox.
 *
 * The kernel calls `me::compose::resolve_landmark_bboxes_from_file`
 * for the bbox span (file-mode resolver — JSON sidecar). The runtime
 * variant of landmark_resolver (calling an inference `Runtime`) is
 * a future cycle's wiring; this stage's `landmark_asset_uri` is
 * expected to point at a pre-computed file fixture for now.
 *
 * Kernel determinism: pure CPU byte-arithmetic (block-mean for
 * Pixelate, integer box-filter for Blur) — VISION §3.1 byte-identity
 * preserved. `time_invariant=false` because the resolver samples
 * per `frame_t`.
 */
#pragma once

namespace me::compose {

void register_face_mosaic_kind();

}  // namespace me::compose
