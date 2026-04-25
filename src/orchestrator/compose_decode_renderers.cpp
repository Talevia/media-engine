/*
 * Implementation of the synthetic-clip renderer helpers.
 * See compose_decode_renderers.hpp for the contract + extraction
 * rationale (debt-split-compose-decode-loop-cpp).
 */
#include "orchestrator/compose_decode_renderers.hpp"

#include "timeline/timeline_impl.hpp"

#ifdef ME_HAS_SKIA
#include "text/text_renderer.hpp"
#endif

#ifdef ME_HAS_LIBASS
#include "text/subtitle_renderer.hpp"

#include <fstream>
#include <sstream>
#include <string_view>
#endif

namespace me::orchestrator {

#ifdef ME_HAS_SKIA
TextRenderResult try_render_text_clip(
    const me::Clip&                            clip,
    me_rational_t                              T,
    int                                        W,
    int                                        H,
    std::vector<std::uint8_t>&                 track_rgba,
    std::unique_ptr<me::text::TextRenderer>&   cache_slot) {

    TextRenderResult r;
    if (clip.type != me::ClipType::Text || !clip.text_params.has_value()) {
        return r;
    }

    if (!cache_slot) {
        cache_slot = std::make_unique<me::text::TextRenderer>(W, H);
    }
    const std::size_t pitch = static_cast<std::size_t>(W) * 4;
    track_rgba.assign(pitch * static_cast<std::size_t>(H), 0);
    if (cache_slot->valid()) {
        cache_slot->render(*clip.text_params, T, track_rgba.data(), pitch);
    }
    r.handled = true;
    r.src_w   = W;
    r.src_h   = H;
    return r;
}
#endif  /* ME_HAS_SKIA */

#ifdef ME_HAS_LIBASS
SubtitleRenderResult try_render_subtitle_clip(
    const me::Clip&                                clip,
    me_rational_t                                  T,
    int                                            W,
    int                                            H,
    std::vector<std::uint8_t>&                     track_rgba,
    std::unique_ptr<me::text::SubtitleRenderer>&   cache_slot,
    std::string*                                   err) {

    SubtitleRenderResult r;
    if (clip.type != me::ClipType::Subtitle || !clip.subtitle_params.has_value()) {
        return r;
    }

    /* Lazy-init: parse the inline content / file_uri exactly once. */
    if (!cache_slot) {
        cache_slot = std::make_unique<me::text::SubtitleRenderer>(W, H);
        const auto& sp = *clip.subtitle_params;
        /* Source the subtitle bytes either from the inline `content`
         * string or by reading the file referenced by `file_uri`.
         * Loader ensures exactly one is populated. Inline empty
         * content is a valid no-op; file_uri that fails to open is
         * surfaced to err so hosts can diagnose the bad path via
         * me_engine_last_error. */
        std::string bytes;
        if (!sp.content.empty()) {
            bytes = sp.content;
        } else if (!sp.file_uri.empty()) {
            std::string path = sp.file_uri;
            constexpr std::string_view file_prefix{"file://"};
            if (path.size() > file_prefix.size() &&
                path.compare(0, file_prefix.size(), file_prefix) == 0) {
                path = path.substr(file_prefix.size());
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                if (err) {
                    *err = "subtitle file_uri not readable: '" +
                            sp.file_uri + "'";
                }
                r.status = ME_E_IO;
                return r;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            bytes = ss.str();
        }
        if (!bytes.empty()) {
            cache_slot->load_from_memory(bytes,
                sp.codepage.empty() ? nullptr : sp.codepage.c_str());
        }
    }

    const std::size_t pitch = static_cast<std::size_t>(W) * 4;
    track_rgba.assign(pitch * static_cast<std::size_t>(H), 0);
    if (cache_slot->valid()) {
        /* t_ms = T.num * 1000 / T.den; rational input is exact, cast
         * to int64 is the natural libass boundary. */
        const int64_t t_ms = (T.num * 1000) / T.den;
        cache_slot->render_frame(t_ms, track_rgba.data(), pitch);
    }
    r.handled = true;
    r.src_w   = W;
    r.src_h   = H;
    return r;
}
#endif  /* ME_HAS_LIBASS */

}  // namespace me::orchestrator
