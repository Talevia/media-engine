/* `me::compose::register_face_sticker_kind` — register the
 * `TaskKindId::RenderFaceSticker` kernel with the global task
 * registry.
 *
 * M11 `face-sticker-compose-graph-stage-impl`. Per the architectural
 * decision in this session: each effect kind gets its own
 * TaskKindId (sibling pattern to `RenderAffineBlit` /
 * `RenderCrossDissolve` etc.). The kernel reads:
 *
 *   - one RGBA8 frame input (the upstream layer's output);
 *   - properties: `sticker_uri` (string), `landmark_asset_uri`
 *     (string — pre-resolved by compose_compile from
 *     `landmarkAssetId`), `frame_t_num` / `frame_t_den` (rational
 *     for landmark resolver's closest-frame selection),
 *     `scale_x` / `scale_y` (double, default 1.0), `offset_x` /
 *     `offset_y` (double, default 0.0);
 *
 * and produces:
 *
 *   - one RGBA8 frame output with the sticker source-over-blended
 *     onto the input at the resolved landmark bboxes.
 *
 * The kernel internally calls
 * `me::compose::decode_sticker_to_rgba8` and
 * `me::compose::resolve_landmark_bboxes_from_file`. Both are
 * deterministic for fixed inputs (PNG decode + file-mode
 * landmark JSON), so the stage is `time_invariant=false` (depends
 * on frame_t but no other context state).
 *
 * The runtime variant of landmark_resolver (calling an inference
 * `Runtime`) is a future cycle's wiring; this stage's
 * `landmark_asset_uri` is expected to point at a pre-computed
 * file fixture for now.
 */
#pragma once

namespace me::compose {

/* Register the face_sticker kernel + KindInfo with the global
 * task registry. Idempotent (registering twice overwrites with
 * identical info). Called once per process by
 * `me_engine_create`'s init block. */
void register_face_sticker_kind();

}  // namespace me::compose
