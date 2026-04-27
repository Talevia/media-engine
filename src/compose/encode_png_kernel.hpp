/*
 * compose::encode_png kernel registration.
 *
 * Registers TaskKindId::RenderEncodePng. 1×RgbaFrame → 1×ByteBuffer
 * (raw PNG bytes). Internally runs sws_scale RGBA8→RGB24 + libavcodec
 * PNG encode in a single kernel invocation — the same two steps that
 * compose_png_at and me_thumbnail_png each ran inline pre-this-kernel.
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
