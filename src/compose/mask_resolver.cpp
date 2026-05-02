/* mask_resolver impl. See header for the contract.
 *
 * Linear scan over `frames[]` finding the entry whose `t` is
 * closest to the requested `time` — same shape as
 * `landmark_resolver`. Closest-frame selection runs in pure
 * rational arithmetic.
 *
 * Base64 decode goes through libavutil's `av_base64_decode`
 * (LGPL, already in the link graph); the decoded bytes are
 * raw alpha (one uint8_t per pixel, row-major).
 */
#include "compose/mask_resolver.hpp"

#ifdef ME_HAS_INFERENCE
#include "inference/asset_cache.hpp"
#include "inference/runtime.hpp"
#include "inference/runtime_factory.hpp"
#endif

extern "C" {
#include <libavutil/base64.h>
}

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace me::compose {

namespace {

using json = nlohmann::json;

std::string strip_file_scheme(std::string_view uri) {
    constexpr std::string_view kFile = "file://";
    if (uri.starts_with(kFile)) return std::string(uri.substr(kFile.size()));
    return std::string(uri);
}

struct RationalDist {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

RationalDist abs_diff(me_rational_t a, me_rational_t b) {
    const std::int64_t lhs = static_cast<std::int64_t>(a.num) *
                              static_cast<std::int64_t>(b.den);
    const std::int64_t rhs = static_cast<std::int64_t>(b.num) *
                              static_cast<std::int64_t>(a.den);
    const std::int64_t diff = lhs - rhs;
    const std::int64_t den_prod = static_cast<std::int64_t>(a.den) *
                                   static_cast<std::int64_t>(b.den);
    return RationalDist{ diff < 0 ? -diff : diff, den_prod };
}

bool less_than(const RationalDist& a, const RationalDist& b) {
    return a.num * b.den < b.num * a.den;
}

}  // namespace

me_status_t resolve_mask_alpha_from_file(
    std::string_view           file_uri,
    me_rational_t              time,
    int*                       out_width,
    int*                       out_height,
    std::vector<std::uint8_t>* out_alpha,
    std::string*               err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "mask_resolver: " + std::move(msg);
        return s;
    };

    if (!out_width || !out_height || !out_alpha) {
        return fail(ME_E_INVALID_ARG, "out_* pointer is null");
    }
    if (file_uri.empty()) return fail(ME_E_INVALID_ARG, "file_uri is empty");

    auto is_supported_scheme = [](std::string_view u) {
        if (u.starts_with("file://")) return true;
        if (u.starts_with("/"))       return true;
        if (u.starts_with("./") || u.starts_with("../")) return true;
        if (u.find("://") == std::string_view::npos) return true;
        return false;
    };
    if (!is_supported_scheme(file_uri)) {
        return fail(ME_E_UNSUPPORTED,
                    "uri scheme not supported (got '" + std::string(file_uri) +
                    "', expected file:// or path)");
    }

    const std::string path = strip_file_scheme(file_uri);

    std::ifstream f(path);
    if (!f.is_open()) {
        return fail(ME_E_IO, "open '" + path + "': failed");
    }
    std::stringstream buf;
    buf << f.rdbuf();
    if (f.bad()) {
        return fail(ME_E_IO, "read '" + path + "': failed");
    }

    json j;
    try {
        j = json::parse(buf.str());
    } catch (const json::parse_error& e) {
        return fail(ME_E_PARSE, "json parse '" + path + "': " + e.what());
    }

    if (!j.is_object() || !j.contains("frames") || !j["frames"].is_array()) {
        return fail(ME_E_PARSE,
                    "json '" + path + "': required object with 'frames' array");
    }

    const auto& frames = j["frames"];
    *out_width  = 0;
    *out_height = 0;
    out_alpha->clear();
    if (frames.empty()) {
        return ME_OK;  /* legitimate "no mask available" representation */
    }

    /* Linear scan for the frame closest to `time`. */
    std::size_t best_idx = 0;
    RationalDist best_dist;
    bool         have_best = false;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& fr = frames[i];
        if (!fr.is_object() || !fr.contains("t") || !fr["t"].is_object()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) + "]: missing 't' object");
        }
        const auto& t_obj = fr["t"];
        if (!t_obj.contains("num") || !t_obj.contains("den") ||
            !t_obj["num"].is_number_integer() || !t_obj["den"].is_number_integer()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) +
                        "].t: required {num, den} integer object");
        }
        const std::int64_t den = t_obj["den"].get<std::int64_t>();
        if (den <= 0) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) + "].t.den must be > 0");
        }
        me_rational_t fr_t{
            t_obj["num"].get<std::int64_t>(), den
        };
        RationalDist dist = abs_diff(fr_t, time);
        if (!have_best || less_than(dist, best_dist)) {
            best_dist = dist;
            best_idx  = i;
            have_best = true;
        }
    }

    /* Decode the best frame's mask. */
    const auto& best_frame = frames[best_idx];
    if (!best_frame.contains("width") || !best_frame.contains("height") ||
        !best_frame.contains("alphaB64") ||
        !best_frame["width"].is_number_integer() ||
        !best_frame["height"].is_number_integer() ||
        !best_frame["alphaB64"].is_string()) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: required {width, height, alphaB64} fields");
    }
    const int w = best_frame["width"].get<int>();
    const int h = best_frame["height"].get<int>();
    if (w <= 0 || h <= 0) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: width / height must be positive");
    }
    const std::string b64 = best_frame["alphaB64"].get<std::string>();
    const std::size_t expected_bytes =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    /* `av_base64_decode` requires the output buffer to be at least
     * 3/4 the input length. AV_BASE64_DECODE_SIZE rounds DOWN, so
     * over-allocate slightly for safety. */
    std::vector<std::uint8_t> alpha_buf(
        expected_bytes + 4);  /* +4 to absorb base64 padding rounding */
    int decoded = av_base64_decode(alpha_buf.data(), b64.c_str(),
                                    static_cast<int>(alpha_buf.size()));
    if (decoded < 0) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "].alphaB64: base64 decode failed");
    }
    if (static_cast<std::size_t>(decoded) != expected_bytes) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "].alphaB64: decoded " + std::to_string(decoded) +
                    " bytes but expected " + std::to_string(expected_bytes) +
                    " (width*height)");
    }
    alpha_buf.resize(expected_bytes);
    *out_width  = w;
    *out_height = h;
    *out_alpha  = std::move(alpha_buf);
    return ME_OK;
}

#ifdef ME_HAS_INFERENCE

namespace {

/* Parse "model:<id>/<version>/<quantization>" → 3-tuple. Same
 * shape as the landmark resolver's parser; sibling stages use
 * the same URI form for both `landmark` and `mask` asset
 * runtime mode. */
bool parse_model_uri(std::string_view uri,
                     std::string*     model_id,
                     std::string*     model_version,
                     std::string*     quantization) {
    constexpr std::string_view kPrefix = "model:";
    if (!uri.starts_with(kPrefix)) return false;
    const std::string_view tail = uri.substr(kPrefix.size());

    const auto first  = tail.find('/');
    if (first == std::string_view::npos) return false;
    const auto second = tail.find('/', first + 1);
    if (second == std::string_view::npos) return false;
    if (tail.find('/', second + 1) != std::string_view::npos) return false;

    *model_id      = std::string(tail.substr(0, first));
    *model_version = std::string(tail.substr(first + 1, second - first - 1));
    *quantization  = std::string(tail.substr(second + 1));
    return !model_id->empty() && !model_version->empty() && !quantization->empty();
}

}  // namespace

me_status_t resolve_mask_alpha_runtime(
    me_engine*                 engine,
    std::string_view           model_uri,
    me_rational_t              /*frame_t*/,
    int                        /*frame_width*/,
    int                        /*frame_height*/,
    int*                       out_mask_width,
    int*                       out_mask_height,
    std::vector<std::uint8_t>* out_alpha,
    std::string*               err) {
    if (!engine || !out_mask_width || !out_mask_height || !out_alpha) {
        return ME_E_INVALID_ARG;
    }
    *out_mask_width  = 0;
    *out_mask_height = 0;
    out_alpha->clear();

    std::string model_id, model_version, quantization;
    if (!parse_model_uri(model_uri, &model_id, &model_version, &quantization)) {
        if (err) *err = "resolve_mask_alpha_runtime: model_uri must match "
                        "'model:<id>/<version>/<quantization>' shape (got '" +
                        std::string(model_uri) + "')";
        return ME_E_INVALID_ARG;
    }

    /* Step 1: factory acquires the validated Runtime — same
     * gates as the landmark sibling: load_model_blob (license
     * whitelist + content_hash) + engine cache. */
    me::inference::Runtime* runtime = nullptr;
    me_status_t s = me::inference::make_runtime_for_model(
        engine,
        model_id.c_str(), model_version.c_str(), quantization.c_str(),
        &runtime, err);
    if (s != ME_OK) return s;

    /* Step 2: build a synthetic input tensor matching
     * SelfieSegmentation's documented 256×256×3 NCHW float32
     * shape. Real frame preprocessing (resize + planar +
     * normalize) is the
     * `mask-resolver-runtime-input-preprocess-impl` follow-up. */
    me::inference::Tensor input;
    input.shape = { 1, 3, 256, 256 };
    input.dtype = me::inference::Dtype::Float32;
    input.bytes.assign(
        static_cast<std::size_t>(1) * 3 * 256 * 256 * 4, 0);

    std::map<std::string, me::inference::Tensor> inputs;
    inputs["input"] = std::move(input);

    /* Step 3: run_cached — exercises the engine's AssetCache
     * (M11 §137) with the per-model identity + input hash. */
    std::map<std::string, me::inference::Tensor> outputs;
    s = me::inference::run_cached(
        engine, *runtime,
        model_id, model_version, quantization,
        inputs, &outputs, err);
    if (s != ME_OK) return s;

    /* Step 4: decode outputs → alpha plane. Model-specific
     * (SelfieSegmentation: sigmoid the logit channel,
     * quantize to uint8, upscale to frame dims). Stub returns
     * ME_E_UNSUPPORTED with the named follow-up bullet.
     *
     * LEGIT: skeleton decode pending — see
     * `selfie-segmentation-mask-decode-impl` BACKLOG bullet. */
    if (err) *err = "resolve_mask_alpha_runtime: SelfieSegmentation mask "
                    "decode pending (see BACKLOG: selfie-segmentation-"
                    "mask-decode-impl); the run_cached + license-whitelist + "
                    "content_hash gates ran successfully (model_id='" +
                    model_id + "')";
    return ME_E_UNSUPPORTED;
}

#endif /* ME_HAS_INFERENCE */

}  // namespace me::compose
