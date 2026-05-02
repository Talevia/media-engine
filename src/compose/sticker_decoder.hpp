/* `me::compose::decode_sticker_to_rgba8` — load a still-image
 * sticker (PNG / WebP / JPEG) from a URI into an RGBA8 buffer.
 *
 * M11 `face-sticker-compose-stage-wiring`. The face_sticker /
 * face_mosaic kernels operate over pre-decoded RGBA8 sticker
 * pixels (see `face_sticker_kernel.hpp`); this TU is the upstream
 * resolver that turns a `params.sticker_uri` string into those
 * pixels. Pure libavformat-via-RAII, no GPL paths.
 *
 * Format support is whatever libavformat's image-2 demuxer +
 * libavcodec's still-image decoders ship with — PNG and WebP are
 * the M11 ship-path formats; JPEG / TIFF / GIF (single-frame) are
 * incidentally supported because libavformat handles them too.
 * Animated formats (GIF / WebP-animation) sample the first frame
 * only — multi-frame sticker support is a future deepening.
 *
 * URI scheme:
 *   - `file:///path/to/sticker.png` — strip the `file://` prefix
 *     and treat the rest as a filesystem path.
 *   - `/absolute/path/to/sticker.png` — path-as-uri shorthand.
 *   - Other schemes (http / https / asset / ...) are not yet
 *     supported and return ME_E_UNSUPPORTED with a named diag.
 *
 * Determinism. PNG / WebP decoding is deterministic per
 * VISION §3.1; the sticker bytes produce the same RGBA8 output
 * across hosts. swscale RGB→RGBA conversion uses BILINEAR (the
 * library default for RGB↔RGBA which is in fact a per-pixel
 * format swap, not a spatial filter) so no inter-pixel
 * interpolation enters the pipeline.
 */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace me::compose {

struct StickerImage {
    int                       width  = 0;
    int                       height = 0;
    /* Tight RGBA8 pixels: `pixels.size() == width * height * 4`.
     * Stride is `width * 4` (no row padding). */
    std::vector<std::uint8_t> pixels;
};

/* Decode the sticker at `uri` into RGBA8. Caller-owned buffer in
 * `*out`; previous contents replaced. On non-OK return, `*out` is
 * left in an undefined-but-safe state (either unchanged or
 * partially populated — caller should treat as garbage).
 *
 * Return codes:
 *   - ME_OK              — `out->pixels` populated.
 *   - ME_E_INVALID_ARG   — null pointer args, empty URI.
 *   - ME_E_UNSUPPORTED   — unsupported URI scheme, decoder
 *                          missing in this libavcodec build.
 *   - ME_E_IO            — file open / read failure.
 *   - ME_E_DECODE        — bytes don't parse as a known image
 *                          format, or decode failed mid-stream.
 *   - ME_E_OUT_OF_MEMORY — alloc failure (frame, sws context). */
me_status_t decode_sticker_to_rgba8(std::string_view  uri,
                                     StickerImage*     out,
                                     std::string*      err);

}  // namespace me::compose
