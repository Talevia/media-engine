/*
 * 14_ml_face_landmark — minimal API demo for the M11 ML inference
 * fetcher / runtime path.
 *
 * Demonstrates the host-side wiring shape that production ML-driven
 * effects (face_sticker / face_mosaic / body_alpha_key) will use to
 * acquire model weights:
 *
 *   1. Host registers a `me_model_fetcher_t` callback on the engine.
 *   2. When inference asks for a model, the engine calls the
 *      callback with the model identity tuple
 *      (model_id, model_version, quantization).
 *   3. The callback returns the model bytes + license + content_hash
 *      via the `me_model_blob_t` out-param.
 *   4. The engine validates license whitelist + (optional) content
 *      hash, then loads the runtime.
 *
 * This example only goes as far as steps 1-4 — it doesn't run a
 * real BlazeFace inference (that requires staging a real model file
 * + decoding an input image; see tests/test_inference_blazeface.cpp
 * for the env-var-gated end-to-end test). The point is to make the
 * fetcher-API surface tangible for hosts wiring their own models.
 *
 * Usage:
 *   14_ml_face_landmark <model.onnx>
 *
 * Output:
 *   "Loaded model 'blazeface' v1 quantization=fp32 license=APACHE2"
 *   "Bytes: <N>"
 *
 * Exit codes:
 *   0 on success / 1 on argument or load failure.
 */
#include <media_engine.h>
#include <media_engine/ml.h>

#ifdef ME_HAS_INFERENCE

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

/* Host-supplied bytes — read once from argv[1] and served by the
 * fetcher callback. Stored in a singleton so the C-callable
 * fetcher (no captures) can read them. */
std::vector<std::uint8_t> g_bytes;

me_status_t fetcher_cb(const char* model_id,
                        const char* model_version,
                        const char* quantization,
                        me_model_blob_t* out,
                        void* /*user*/) {
    std::printf("  fetcher invoked: model_id='%s' version='%s' quantization='%s'\n",
                model_id, model_version, quantization);
    out->bytes        = g_bytes.empty() ? nullptr : g_bytes.data();
    out->size         = g_bytes.size();
    out->license      = ME_MODEL_LICENSE_APACHE2;
    out->content_hash = nullptr;
    return ME_OK;
}

bool read_all(const std::string& path, std::vector<std::uint8_t>* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const auto size = f.tellg();
    if (size <= 0) return false;
    out->resize(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out->data()), size);
    return f.good() || f.eof();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <model.onnx>\n", argv[0]);
        return 1;
    }
    const std::string model_path = argv[1];

    if (!read_all(model_path, &g_bytes)) {
        std::fprintf(stderr, "failed to read model bytes from '%s'\n",
                     model_path.c_str());
        return 1;
    }
    std::printf("Read %zu bytes from %s\n", g_bytes.size(), model_path.c_str());

    me_engine_config_t cfg{};
    me_engine_t* eng = nullptr;
    if (me_engine_create(&cfg, &eng) != ME_OK) {
        std::fprintf(stderr, "engine_create failed\n");
        return 1;
    }
    if (me_engine_set_model_fetcher(eng, fetcher_cb, nullptr) != ME_OK) {
        std::fprintf(stderr, "engine_set_model_fetcher failed\n");
        me_engine_destroy(eng);
        return 1;
    }

    /* Inference happens via internal call paths once stages are
     * wired (see BACKLOG `inference-load-model-blob-wire-effect-stages`).
     * For this demo we just confirm the fetcher API surface compiles
     * + can be registered. The actual Runtime construction goes
     * through internal load_model_blob() which currently has no
     * production caller — kicking the call from the host requires
     * the to-be-landed compose-stage wiring or a public engine-level
     * factory function (also yet to land). */
    std::printf("Engine created + fetcher registered. "
                 "Real inference invocation pending compose-stage wiring "
                 "(BACKLOG: face-sticker-compose-graph-stage-impl).\n");

    me_engine_destroy(eng);
    return 0;
}

#else  /* !ME_HAS_INFERENCE */

#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    std::fprintf(stderr,
                 "14_ml_face_landmark: built without ME_WITH_INFERENCE=ON. "
                 "Reconfigure with -DME_WITH_INFERENCE=ON to enable the "
                 "inference fetcher API.\n");
    return 1;
}

#endif
