/*
 * Synthetic-clip renderer helpers for the compose video frame loop.
 * Extracted from compose_decode_loop.cpp (387 → ~310 lines) to keep
 * the per-frame loop focused on the decode + composite shape and to
 * give the Skia text-clip branch and the libass subtitle-clip branch
 * each their own callable surface. Both branches are gated by their
 * respective compile defs (ME_HAS_SKIA / ME_HAS_LIBASS) so the
 * helpers compile to nothing in builds that disable Skia or libass.
 *
 * "Synthetic" = no decoder; the renderer paints into the per-track
 * RGBA scratch buffer directly. The caller treats the clip as having
 * source dimensions equal to the output canvas (W×H) and applies
 * Transform / opacity downstream the same way decoded clips are
 * composited.
 *
 * Cache-slot ownership: the loop owns one
 * `std::unique_ptr<*Renderer>` per Timeline::clips index. On first
 * visit the helper allocates the renderer (lazy init keeps cost off
 * non-text/non-subtitle timelines). Subsequent visits reuse the
 * already-built renderer + cached parsed track.
 */
#pragma once

#include <media_engine/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace me { struct Clip; }

#ifdef ME_HAS_SKIA
namespace me::text { class TextRenderer; }
#endif
#ifdef ME_HAS_LIBASS
namespace me::text { class SubtitleRenderer; }
#endif

namespace me::orchestrator {

#ifdef ME_HAS_SKIA
struct TextRenderResult {
    bool handled = false;   /* true iff the clip was a text clip and was painted */
    int  src_w   = 0;       /* always W when handled */
    int  src_h   = 0;       /* always H when handled */
};

/* Paints the text clip into `track_rgba` (zero-initialised by this
 * helper before drawing). Lazily constructs `cache_slot` on first
 * call. Returns handled=false when `clip` is not a text clip with
 * `text_params` populated — the caller then falls through to the
 * decoder path.  */
TextRenderResult try_render_text_clip(
    const me::Clip&                            clip,
    me_rational_t                              T,
    int                                        W,
    int                                        H,
    std::vector<std::uint8_t>&                 track_rgba,
    std::unique_ptr<me::text::TextRenderer>&   cache_slot);
#endif  /* ME_HAS_SKIA */

#ifdef ME_HAS_LIBASS
struct SubtitleRenderResult {
    bool        handled = false;
    int         src_w   = 0;
    int         src_h   = 0;
    /* Surfaces the file_uri-read error path so the caller can return
     * the same status to its own caller. ME_OK + handled==false means
     * "this isn't a subtitle clip". ME_OK + handled==true means a
     * paint happened. ME_E_IO means file_uri was set but unreadable;
     * the caller should return ME_E_IO and `*err` is already populated. */
    me_status_t status  = ME_OK;
};

SubtitleRenderResult try_render_subtitle_clip(
    const me::Clip&                                clip,
    me_rational_t                                  T,
    int                                            W,
    int                                            H,
    std::vector<std::uint8_t>&                     track_rgba,
    std::unique_ptr<me::text::SubtitleRenderer>&   cache_slot,
    std::string*                                   err);
#endif  /* ME_HAS_LIBASS */

}  // namespace me::orchestrator
