/*
 * compose::convert_rgba8 kernel registration.
 *
 * Registers TaskKindId::RenderConvertRgba8. Wraps `frame_to_rgba8`
 * (sws_scale-based AVFrame → tightly-packed RGBA8 conversion) into the
 * graph kernel ABI.
 *
 * Schema:
 *   inputs:  [frame: AvFrameHandle]
 *   outputs: [rgba:  RgbaFrame]
 *   params:  (none)
 *
 * time_invariant = true: the conversion is purely a function of the
 * input AVFrame's pixels, so two evaluations sharing the same input
 * cache to the same output.
 */
#pragma once

namespace me::compose {
void register_convert_rgba8_kind();
}
