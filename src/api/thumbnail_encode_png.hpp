/*
 * Internal helper for src/api/thumbnail.cpp — wraps the PNG
 * encoder branch (avcodec_find_encoder(AV_CODEC_ID_PNG) + open +
 * send_frame + receive_packet + malloc copy) so the public
 * me_thumbnail_png entrypoint stays focused on probing / decoding
 * / sws conversion.
 *
 * Pre-split, thumbnail.cpp was 341 lines and one feature commit
 * away from §1a's 400-line ceiling. Splitting the encoder branch
 * out gives both files comfortable headroom for the next thumbnail
 * format (webp / avif) without immediately re-tripping the debt
 * scanner.
 *
 * This header is private (under src/), never shipped. Public
 * callers continue to go through include/media_engine/thumbnail.h.
 */
#pragma once

#include <media_engine/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "io/ffmpeg_raii.hpp"
#include "resource/codec_pool.hpp"

namespace me::detail {

/* Encode the RGB24 frame `rgb` as a single PNG packet, allocating
 * the byte buffer with malloc so me_buffer_free (→ std::free) can
 * release it symmetrically. Returns ME_OK on success with `*out_png`
 * + `*out_size` populated; on failure, writes a diagnostic to `err`
 * (caller surfaces via me_engine_last_error) and returns the
 * appropriate me_status_t.
 *
 * `pool` is borrowed (engine->codecs); may be nullptr — in that
 * case we fall back to a CodecPool::Ptr with a no-op deleter so the
 * AVCodecContext leaks until the encoder finalizes (matches the
 * pre-split fallback). */
me_status_t encode_rgb_to_png(const me::io::AvFramePtr& rgb,
                               int                       out_w,
                               int                       out_h,
                               me::resource::CodecPool*  pool,
                               std::uint8_t**            out_png,
                               std::size_t*              out_size,
                               std::string*              err);

}  // namespace me::detail
