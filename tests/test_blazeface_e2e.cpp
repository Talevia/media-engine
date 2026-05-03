/* test_blazeface_e2e — end-to-end BlazeFace pipeline test
 * driving `resolve_landmark_bboxes_runtime` against a real
 * `.onnx` model file + a real face image.
 *
 * Sibling of `test_inference_blazeface.cpp` (which covers
 * runtime-loading + parity at the inference-only layer);
 * this suite exercises the full chain:
 *   prepare_blazeface_input → run_cached → decode_blazeface_bboxes
 *
 * Env-var gates (BOTH required to run):
 *   ME_TEST_BLAZEFACE_ONNX_PATH    — `.onnx` model bytes path.
 *   ME_TEST_BLAZEFACE_FACE_PNG_PATH — RGBA-decodable image
 *                                     containing at least one
 *                                     face. PNG / JPEG / WebP
 *                                     all OK (decoded via the
 *                                     existing sticker_decoder
 *                                     pipeline).
 *
 * When either env var is unset (CI default), the TEST_CASE
 * fast-paths to a skipped no-op — model bytes are NOT bundled
 * in the engine binary per VISION §3.5. Hosts staging the
 * test point both env vars at local files.
 *
 * Asserts (when both env vars set):
 *   1. `resolve_landmark_bboxes_runtime` returns ME_OK.
 *   2. At least one Bbox is detected.
 *   3. Detected bbox has positive width + height (sanity).
 *
 * Gated on `ME_HAS_INFERENCE` (the test depends on the runtime
 * factory being compiled in). Without inference compiled the
 * test fast-skips at compile time. */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#if defined(ME_HAS_INFERENCE)

#include "compose/bbox.hpp"
#include "compose/landmark_resolver.hpp"
#include "compose/sticker_decoder.hpp"
#include "media_engine/engine.h"
#include "media_engine/ml.h"

#include <cstdint>
#include <cstdio>
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
    out_blob->content_hash = nullptr;  /* host-attested via env-var */
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

TEST_CASE("BlazeFace e2e: real-model + real-face → bbox(es) detected") {
    const char* model_path = env_or_null("ME_TEST_BLAZEFACE_ONNX_PATH");
    const char* face_path  = env_or_null("ME_TEST_BLAZEFACE_FACE_PNG_PATH");
    if (!model_path || !face_path) {
        MESSAGE("skipping: ME_TEST_BLAZEFACE_ONNX_PATH and/or "
                "ME_TEST_BLAZEFACE_FACE_PNG_PATH not set");
        return;
    }

    /* Stage 1: read model bytes. */
    REQUIRE(read_file_bytes(model_path, &g_model_bytes));
    REQUIRE(!g_model_bytes.empty());

    /* Stage 2: decode the face image to RGBA8 via the existing
     * sticker_decoder (handles PNG / JPEG / WebP transparently). */
    me::compose::StickerImage face;
    std::string               decode_err;
    const std::string         file_uri = std::string{"file://"} + face_path;
    REQUIRE(me::compose::decode_sticker_to_rgba8(
                file_uri, &face, &decode_err) == ME_OK);
    REQUIRE(face.width  > 0);
    REQUIRE(face.height > 0);
    REQUIRE(face.pixels.size() ==
            static_cast<std::size_t>(face.width) * face.height * 4);

    /* Stage 3: engine + fetcher. */
    me_engine_config_t cfg{};
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(&cfg, &eng) == ME_OK);
    REQUIRE(me_engine_set_model_fetcher(eng, env_fetcher, nullptr) == ME_OK);

    /* Stage 4: drive resolve_landmark_bboxes_runtime end-to-end.
     * The resolver will: (a) load the model via the fetcher,
     * (b) build a BlazeFace input tensor from the RGBA frame
     * bytes, (c) run the model via the cached runtime,
     * (d) decode via decode_blazeface_bboxes. */
    std::vector<me::compose::Bbox> bboxes;
    std::string                    err;
    const std::size_t stride = static_cast<std::size_t>(face.width) * 4;
    const me_status_t s = me::compose::resolve_landmark_bboxes_runtime(
        eng, "model:blazeface/v2/fp32",
        me_rational_t{0, 30}, face.width, face.height,
        face.pixels.data(), stride,
        &bboxes, &err);

    /* On hosts without a backend (no CoreML, no ONNX), the
     * factory fails before reaching the decode. Treat as a
     * pass-with-message so the suite stays usable on those
     * hosts without manually env-gating. */
    if (s == ME_E_INTERNAL && err.find("inference runtime") != std::string::npos) {
        MESSAGE("skipping: no compiled-in runtime backend (CoreML / ONNX)");
        me_engine_destroy(eng);
        return;
    }

    REQUIRE_MESSAGE(s == ME_OK, "resolver returned " << me_status_str(s) << ": " << err);
    CHECK_MESSAGE(!bboxes.empty(),
                   "expected at least one face detected; check that "
                   << face_path << " contains a clear frontal face");
    for (const auto& b : bboxes) {
        CHECK(b.width()  > 0);
        CHECK(b.height() > 0);
    }

    me_engine_destroy(eng);
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("BlazeFace e2e: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub mirroring the other inference test
     * suites. */
}

#endif
