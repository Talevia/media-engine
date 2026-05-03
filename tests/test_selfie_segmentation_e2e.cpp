/* test_selfie_segmentation_e2e — end-to-end SelfieSegmentation
 * pipeline test driving `resolve_mask_alpha_runtime` against a
 * real `.onnx` model file + a real portrait image.
 *
 * Sibling of `test_blazeface_e2e.cpp` (full-pipeline coverage
 * for the face-detection variant). This suite covers the
 * portrait-segmentation variant of the M11 ML pipeline:
 *   prepare_selfie_segmentation_input → run_cached
 *   → decode_selfie_segmentation_mask
 *
 * Env-var gates (BOTH required to run):
 *   ME_TEST_SELFIE_SEG_ONNX_PATH    — `.onnx` model bytes path.
 *   ME_TEST_SELFIE_SEG_PORTRAIT_PATH — RGBA-decodable portrait
 *                                      image (PNG / JPEG / WebP).
 *
 * When either env var is unset (CI default), the TEST_CASE
 * fast-paths to a skipped no-op — model bytes are NOT bundled
 * per VISION §3.5.
 *
 * Asserts (when both env vars set):
 *   1. `resolve_mask_alpha_runtime` returns ME_OK.
 *   2. Mask dimensions match the input frame (the resolver
 *      bilinear-upscales to frame coords).
 *   3. The alpha plane has at least one non-extremal byte
 *      (not all 0 or all 255 — the model produced a real
 *      foreground/background split).
 *
 * Gated on `ME_HAS_INFERENCE`. */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#if defined(ME_HAS_INFERENCE)

#include "compose/mask_resolver.hpp"
#include "compose/sticker_decoder.hpp"
#include "media_engine/engine.h"
#include "media_engine/ml.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> g_model_bytes;

me_status_t env_fetcher(const char*       /*model_id*/,
                         const char*       /*model_version*/,
                         const char*       /*quantization*/,
                         me_model_blob_t*  out_blob,
                         void*             /*user*/) {
    out_blob->bytes        = g_model_bytes.empty() ? nullptr : g_model_bytes.data();
    out_blob->size         = g_model_bytes.size();
    out_blob->license      = ME_MODEL_LICENSE_APACHE2;
    out_blob->content_hash = nullptr;
    return ME_OK;
}

bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size <= 0) return false;
    out->resize(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out->data()), size);
    return f.good() || f.eof();
}

const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return nullptr;
    return v;
}

}  // namespace

TEST_CASE("SelfieSegmentation e2e: real-model + real-portrait → mask plane") {
    const char* model_path    = env_or_null("ME_TEST_SELFIE_SEG_ONNX_PATH");
    const char* portrait_path = env_or_null("ME_TEST_SELFIE_SEG_PORTRAIT_PATH");
    if (!model_path || !portrait_path) {
        MESSAGE("skipping: ME_TEST_SELFIE_SEG_ONNX_PATH and/or "
                "ME_TEST_SELFIE_SEG_PORTRAIT_PATH not set");
        return;
    }

    /* Stage 1: read model bytes. */
    REQUIRE(read_file_bytes(model_path, &g_model_bytes));
    REQUIRE(!g_model_bytes.empty());

    /* Stage 2: decode the portrait image to RGBA8. */
    me::compose::StickerImage portrait;
    std::string               decode_err;
    const std::string         file_uri = std::string{"file://"} + portrait_path;
    REQUIRE(me::compose::decode_sticker_to_rgba8(
                file_uri, &portrait, &decode_err) == ME_OK);
    REQUIRE(portrait.width  > 0);
    REQUIRE(portrait.height > 0);
    REQUIRE(portrait.pixels.size() ==
            static_cast<std::size_t>(portrait.width) * portrait.height * 4);

    /* Stage 3: engine + fetcher. */
    me_engine_config_t cfg{};
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(&cfg, &eng) == ME_OK);
    REQUIRE(me_engine_set_model_fetcher(eng, env_fetcher, nullptr) == ME_OK);

    /* Stage 4: drive resolve_mask_alpha_runtime end-to-end.
     * The resolver chains preprocess (256×256 RGBA → CHW
     * float32 [0, 1]) + run_cached + decode (sigmoid + uint8
     * quantize + bilinear upscale to portrait dims). */
    int                       mask_w = 0;
    int                       mask_h = 0;
    std::vector<std::uint8_t> alpha;
    std::string               err;
    const std::size_t stride = static_cast<std::size_t>(portrait.width) * 4;
    const me_status_t s = me::compose::resolve_mask_alpha_runtime(
        eng, "model:selfie_seg/v3/fp32",
        me_rational_t{0, 30}, portrait.width, portrait.height,
        portrait.pixels.data(), stride,
        &mask_w, &mask_h, &alpha, &err);

    /* Hosts without a runtime backend → graceful skip. */
    if (s == ME_E_INTERNAL && err.find("inference runtime") != std::string::npos) {
        MESSAGE("skipping: no compiled-in runtime backend (CoreML / ONNX)");
        me_engine_destroy(eng);
        return;
    }

    REQUIRE_MESSAGE(s == ME_OK, "resolver returned " << me_status_str(s) << ": " << err);
    CHECK(mask_w == portrait.width);
    CHECK(mask_h == portrait.height);
    REQUIRE(alpha.size() ==
            static_cast<std::size_t>(mask_w) * mask_h);

    /* The mask should have a real foreground/background split —
     * not entirely 0 (everything background) and not entirely
     * 255 (everything foreground). */
    bool found_low  = false;
    bool found_high = false;
    bool found_mid  = false;
    for (auto v : alpha) {
        if (v < 32)         found_low  = true;
        if (v > 224)        found_high = true;
        if (v >= 32 && v <= 224) found_mid = true;
        if (found_low && found_high) break;
    }
    CHECK_MESSAGE(found_low,  "expected at least some background pixels (alpha < 32)");
    CHECK_MESSAGE(found_high, "expected at least some foreground pixels (alpha > 224)");
    /* `found_mid` is sanity — the bilinear upscale produces
     * intermediate values at the foreground-background boundary. */
    CHECK_MESSAGE(found_mid,
                   "expected at least some boundary pixels (alpha 32..224); "
                   "bilinear upscale should produce them");

    me_engine_destroy(eng);
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("SelfieSegmentation e2e: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub. */
}

#endif
