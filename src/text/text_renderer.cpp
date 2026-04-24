#include "text/text_renderer.hpp"

#include "text/skia_backend.hpp"

#include <cstdint>
#include <cstring>

namespace me::text {

namespace {

std::uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0;
}

std::uint8_t hex_byte(char hi, char lo) {
    return static_cast<std::uint8_t>((hex_nibble(hi) << 4) | hex_nibble(lo));
}

}  // namespace

void TextRenderer::parse_hex_rgba(const std::string& hex,
                                    std::uint8_t& r, std::uint8_t& g,
                                    std::uint8_t& b, std::uint8_t& a) {
    /* Retained for unit-test compatibility + host debugging. The
     * IR now stores color as `me::AnimatedColor`; render() pulls
     * its evaluated RGBA directly without round-tripping through
     * the hex string. Invalid strings → opaque white (defensive). */
    r = g = b = 0xFF;
    a = 0xFF;
    if (hex.size() != 7 && hex.size() != 9) return;
    if (hex[0] != '#') return;
    r = hex_byte(hex[1], hex[2]);
    g = hex_byte(hex[3], hex[4]);
    b = hex_byte(hex[5], hex[6]);
    if (hex.size() == 9) {
        a = hex_byte(hex[7], hex[8]);
    }
}

TextRenderer::TextRenderer(int canvas_w, int canvas_h)
    : backend_(std::make_unique<SkiaBackend>(canvas_w, canvas_h)) {}

TextRenderer::~TextRenderer() = default;

bool TextRenderer::valid() const noexcept {
    return backend_ && backend_->valid();
}

void TextRenderer::render(const me::TextClipParams& params,
                           me_rational_t             t,
                           std::uint8_t*             out_rgba,
                           std::size_t               stride_bytes) {
    if (!valid() || !out_rgba) return;

    /* Clear to fully transparent — callers alpha-over this onto
     * another layer, so any existing pixel contents would
     * double-blend. */
    backend_->clear(0, 0, 0, 0);

    /* Evaluate animated fields at t. font_size / x / y are
     * AnimatedNumber → double; color is AnimatedColor → RGBA bytes.
     * Skia expects float for font_size / x / y. */
    const double font_size_d = params.font_size.evaluate_at(t);
    const double x_d         = params.x.evaluate_at(t);
    const double y_d         = params.y.evaluate_at(t);
    const auto   rgba        = params.color.evaluate_at(t);

    backend_->draw_string(params.content,
                          static_cast<float>(x_d),
                          static_cast<float>(y_d),
                          static_cast<float>(font_size_d),
                          rgba[0], rgba[1], rgba[2], rgba[3]);

    backend_->read_pixels(out_rgba, stride_bytes);
}

}  // namespace me::text
