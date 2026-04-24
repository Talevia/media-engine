/*
 * me::text::SubtitleRenderer — libass-backed ASS/SSA subtitle
 * renderer.
 *
 * M5 exit criterion "libass 字幕 track" foundation. Thin RAII
 * wrapper around libass's ASS_Library + ASS_Renderer + ASS_Track
 * handles. Compiled only when `-DME_WITH_LIBASS=ON` (default ON;
 * auto-flips OFF when pkg-config can't find libass — see
 * `src/CMakeLists.txt`).
 *
 * Usage:
 *   SubtitleRenderer r(1920, 1080);
 *   r.load_from_memory(ass_text, ass_bytes);
 *   r.render_frame(time_ms, out_rgba, 1920 * 4);  // stride = w*4
 *
 * Output format: pre-multiplied RGBA8 by default (matches libass's
 * ASS_Image::type == ASS_PLAIN blend contract). Caller supplies
 * the destination buffer — the renderer composites subtitle glyphs
 * onto it (alpha-over blending at each pixel).
 *
 * Threading: libass is thread-unsafe per-renderer. Each thread
 * rendering subtitles needs its own SubtitleRenderer instance, or
 * all accesses must be externally serialized. Matches the one-
 * renderer-per-timeline-render pattern our compose loop already
 * uses.
 *
 * Font handling: libass's internal font selector walks fallbacks
 * for characters the primary font lacks (CJK, emoji). The first
 * landing calls `ass_set_fonts_dir` to find system fonts; future
 * cycle extends to explicit font-file loading + CoreText
 * integration for macOS-native fallback.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/* Opaque forward-decls — ass.h has to be included only in the
 * .cpp. Keeps libass off the include graph of callers. */
struct ass_library;
struct ass_renderer;
struct ass_track;
typedef struct ass_library  ASS_Library;
typedef struct ass_renderer ASS_Renderer;
typedef struct ass_track    ASS_Track;

namespace me::text {

class SubtitleRenderer {
public:
    /* Width / height define the "stage" size libass renders to —
     * typically matches the output video canvas. Aspect-sensitive
     * subtitle positioning ("\an2" bottom-center etc.) uses this. */
    SubtitleRenderer(int width, int height);
    ~SubtitleRenderer();

    SubtitleRenderer(const SubtitleRenderer&)            = delete;
    SubtitleRenderer& operator=(const SubtitleRenderer&) = delete;

    /* Load .ass / .srt content from memory. `codepage` is passed to
     * libass for non-UTF-8 files; nullptr / empty = UTF-8 assumed.
     * Returns true iff track parsed successfully. */
    bool load_from_memory(std::string_view content,
                           const char* codepage = nullptr);

    /* Alpha-composite subtitle bitmap at timeline time `t_ms` onto
     * `out_rgba`. `stride_bytes` is the pitch of the dst buffer
     * (usually width × 4 for tightly-packed RGBA8). Caller owns
     * `out_rgba` + must have W × H × 4 bytes. Input bytes are
     * preserved where no subtitle pixel appears. */
    void render_frame(int64_t         t_ms,
                       std::uint8_t*   out_rgba,
                       std::size_t     stride_bytes);

    /* True when ctor successfully allocated libass state AND a
     * track has been loaded. `render_frame` is a no-op when
     * false — callers can always invoke it safely. */
    bool valid() const noexcept { return valid_; }

    int width()  const noexcept { return width_;  }
    int height() const noexcept { return height_; }

private:
    int            width_    = 0;
    int            height_   = 0;
    bool           valid_    = false;
    ASS_Library*   library_  = nullptr;
    ASS_Renderer*  renderer_ = nullptr;
    ASS_Track*     track_    = nullptr;
};

}  // namespace me::text
