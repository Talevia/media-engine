/* landmark_resolver impl. See header for the contract.
 *
 * Single-pass linear scan over `frames[]` finding the entry whose
 * `t` is closest to the requested `time`. Distance is
 * `|t_frame - time|` in rational arithmetic — no double-precision
 * intermediate, no cross-host SIMD drift. Ties (equidistant
 * frames) pick the first one in document order; callers shouldn't
 * rely on tie-breaking semantics (synthetic fixtures don't have
 * duplicate timestamps).
 */
#include "compose/landmark_resolver.hpp"

#include "compose/inference_input.hpp"

#ifdef ME_HAS_INFERENCE
#include "inference/asset_cache.hpp"
#include "inference/runtime.hpp"
#include "inference/runtime_factory.hpp"
#endif

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

/* Absolute distance |a - b| as a rational. To compare two
 * distances, we cross-multiply: (a/b) < (c/d) iff a*d < c*b
 * (both b, d > 0). Caller compares
 * `dist_lhs.num * dist_rhs.den < dist_rhs.num * dist_lhs.den`. */
struct RationalDist {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

RationalDist abs_diff(me_rational_t a, me_rational_t b) {
    /* a/b - c/d == (a*d - c*b) / (b*d). We want the absolute. */
    const std::int64_t lhs = static_cast<std::int64_t>(a.num) *
                              static_cast<std::int64_t>(b.den);
    const std::int64_t rhs = static_cast<std::int64_t>(b.num) *
                              static_cast<std::int64_t>(a.den);
    const std::int64_t diff = lhs - rhs;
    const std::int64_t den_prod = static_cast<std::int64_t>(a.den) *
                                   static_cast<std::int64_t>(b.den);
    return RationalDist{
        diff < 0 ? -diff : diff,
        den_prod
    };
}

bool less_than(const RationalDist& a, const RationalDist& b) {
    /* a.num/a.den < b.num/b.den iff a.num*b.den < b.num*a.den
     * (both denominators positive). */
    return a.num * b.den < b.num * a.den;
}

}  // namespace

me_status_t resolve_landmark_bboxes_from_file(
    std::string_view  file_uri,
    me_rational_t     time,
    std::vector<Bbox>* out,
    std::string*      err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "landmark_resolver: " + std::move(msg);
        return s;
    };

    if (!out) return fail(ME_E_INVALID_ARG, "out is null");
    if (file_uri.empty()) return fail(ME_E_INVALID_ARG, "file_uri is empty");

    /* URI shape check — same shapes accepted as
     * `decode_sticker_to_rgba8` for consistency. */
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
    out->clear();
    if (frames.empty()) {
        /* No frames at all → no bboxes; legitimate "no landmarks
         * detected anywhere" representation. */
        return ME_OK;
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
                        "frames[" + std::to_string(i) +
                        "].t.den must be > 0");
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

    /* Decode the best frame's bboxes. */
    const auto& best_frame = frames[best_idx];
    if (!best_frame.contains("bboxes") || !best_frame["bboxes"].is_array()) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: missing 'bboxes' array");
    }
    const auto& bboxes = best_frame["bboxes"];
    out->reserve(bboxes.size());
    for (std::size_t i = 0; i < bboxes.size(); ++i) {
        const auto& b = bboxes[i];
        if (!b.is_object() ||
            !b.contains("x0") || !b.contains("y0") ||
            !b.contains("x1") || !b.contains("y1") ||
            !b["x0"].is_number_integer() || !b["y0"].is_number_integer() ||
            !b["x1"].is_number_integer() || !b["y1"].is_number_integer()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(best_idx) +
                        "].bboxes[" + std::to_string(i) +
                        "]: required {x0, y0, x1, y1} integer object");
        }
        Bbox bb;
        bb.x0 = b["x0"].get<std::int32_t>();
        bb.y0 = b["y0"].get<std::int32_t>();
        bb.x1 = b["x1"].get<std::int32_t>();
        bb.y1 = b["y1"].get<std::int32_t>();
        out->push_back(bb);
    }
    return ME_OK;
}

#ifdef ME_HAS_INFERENCE

namespace {

/* Parse "model:<id>/<version>/<quantization>" → 3-tuple. Returns
 * false on shape mismatch. The shape is documented in
 * landmark_resolver.hpp; matching production callers (face
 * sticker / face mosaic stages) construct the URI from the
 * Asset.uri field after detecting the `model:` scheme prefix. */
bool parse_model_uri(std::string_view uri,
                     std::string*     model_id,
                     std::string*     model_version,
                     std::string*     quantization) {
    constexpr std::string_view kPrefix = "model:";
    if (!uri.starts_with(kPrefix)) return false;
    const std::string_view tail = uri.substr(kPrefix.size());

    /* Split on '/' — exactly two slashes expected (id/ver/quant). */
    const auto first  = tail.find('/');
    if (first == std::string_view::npos) return false;
    const auto second = tail.find('/', first + 1);
    if (second == std::string_view::npos) return false;
    /* Reject a third slash — too many segments. */
    if (tail.find('/', second + 1) != std::string_view::npos) return false;

    *model_id      = std::string(tail.substr(0, first));
    *model_version = std::string(tail.substr(first + 1, second - first - 1));
    *quantization  = std::string(tail.substr(second + 1));
    return !model_id->empty() && !model_version->empty() && !quantization->empty();
}

}  // namespace

me_status_t resolve_landmark_bboxes_runtime(
    me_engine*           engine,
    std::string_view     model_uri,
    me_rational_t        /*frame_t*/,
    int                  frame_width,
    int                  frame_height,
    const std::uint8_t*  frame_rgba,
    std::size_t          frame_stride_bytes,
    std::vector<Bbox>*   out,
    std::string*         err) {
    if (!engine || !out) return ME_E_INVALID_ARG;
    out->clear();

    /* Reject obviously malformed (rgba, dims, stride) tuples
     * upfront so the preprocessor's diagnostic doesn't have to
     * fire. NULL rgba is allowed (synthetic-tensor fallback). */
    if (frame_rgba) {
        if (frame_width <= 0 || frame_height <= 0) return ME_E_INVALID_ARG;
        if (frame_stride_bytes < static_cast<std::size_t>(frame_width) * 4) {
            return ME_E_INVALID_ARG;
        }
    }

    std::string model_id, model_version, quantization;
    if (!parse_model_uri(model_uri, &model_id, &model_version, &quantization)) {
        if (err) *err = "resolve_landmark_bboxes_runtime: model_uri must match "
                        "'model:<id>/<version>/<quantization>' shape (got '" +
                        std::string(model_uri) + "')";
        return ME_E_INVALID_ARG;
    }

    /* Step 1: factory acquires the validated Runtime — exercises
     * load_model_blob (license whitelist + content_hash gate)
     * + the engine's loaded_models cache + loaded_runtimes
     * cache. M11 §138 evidence: every call to this function
     * goes through the license-validating helper. */
    me::inference::Runtime* runtime = nullptr;
    me_status_t s = me::inference::make_runtime_for_model(
        engine,
        model_id.c_str(), model_version.c_str(), quantization.c_str(),
        &runtime, err);
    if (s != ME_OK) return s;

    /* Step 2: frame preprocessing. When `frame_rgba` is non-NULL,
     * resize + planar-convert + [-1, 1] normalize via
     * `prepare_blazeface_input`. NULL frame_rgba falls back to a
     * synthetic zero-filled tensor of the documented shape so
     * test callers without real pixels still drive the wire. */
    me::inference::Tensor input;
    if (frame_rgba) {
        const me_status_t pp = prepare_blazeface_input(
            frame_rgba, frame_width, frame_height,
            frame_stride_bytes, &input, err);
        if (pp != ME_OK) return pp;
    } else {
        input.shape = { 1, 3, kBlazefaceInputDim, kBlazefaceInputDim };
        input.dtype = me::inference::Dtype::Float32;
        input.bytes.assign(
            static_cast<std::size_t>(1) * 3 *
            kBlazefaceInputDim * kBlazefaceInputDim * 4, 0);
    }

    std::map<std::string, me::inference::Tensor> inputs;
    inputs["input"] = std::move(input);

    /* Step 3: run_cached — exercises the engine's AssetCache
     * (M11 §137 evidence: every call hits or stores the
     * (model_id, version, quant, input_hash) cache key). */
    std::map<std::string, me::inference::Tensor> outputs;
    s = me::inference::run_cached(
        engine, *runtime,
        model_id, model_version, quantization,
        inputs, &outputs, err);
    if (s != ME_OK) return s;

    /* Step 4: decode outputs → bboxes. Model-specific (BlazeFace
     * anchors, sigmoid + regression decode, NMS). Stub returns
     * ME_E_UNSUPPORTED with the named follow-up bullet. The
     * preceding 3 steps ARE the production wire that closes
     * §137/§138; this branch is the model-decode follow-up.
     *
     * LEGIT: skeleton decode pending — see
     * `blazeface-anchor-decode-impl` BACKLOG bullet for the
     * model-specific decode logic (anchor regression + NMS). */
    if (err) *err = "resolve_landmark_bboxes_runtime: BlazeFace anchor decode "
                    "pending (see BACKLOG: blazeface-anchor-decode-impl); "
                    "the run_cached + license-whitelist + content_hash gates "
                    "ran successfully (model_id='" + model_id + "')";
    return ME_E_UNSUPPORTED;
}

#endif /* ME_HAS_INFERENCE */

}  // namespace me::compose
