/*
 * compose::encode_png kernel registration.
 *
 * Registers TaskKindId::RenderEncodePng. 1×RgbaFrame → 1×ByteBuffer
 * (raw PNG bytes). Wraps the existing me::detail::encode_rgb_to_png
 * helper into the graph kernel ABI so PNG generation becomes a
 * scheduled Node instead of an inline branch in
 * compose_png_at / me_thumbnail_png.
 *
 * Schema:
 *   inputs:  [rgba: RgbaFrame]
 *   outputs: [png:  ByteBuffer]
 *   params:  (none — output dimensions match input pixels;
 *            scale-to-fit happens upstream via RenderAffineBlit
 *            when needed)
 *
 * time_invariant = true: same RGBA8 input → identical PNG output
 * (libavcodec PNG encoder is deterministic at fixed compression
 * level). cacheable = true (default).
 */
#pragma once

namespace me::compose {
void register_encode_png_kind();
}
