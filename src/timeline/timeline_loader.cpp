/* Timeline JSON → IR top-level dispatch.
 *
 * Scope: entry point `load_json`. The heavy lifting lives in three
 * sibling TUs (debt-split-timeline-loader-cpp):
 *   - timeline_loader_assets.cpp        (parse_assets_into)
 *   - timeline_loader_tracks.cpp        (parse_track_into — clip walk)
 *   - timeline_loader_transitions.cpp   (parse_track_transitions_into)
 * All three live in the same `me::timeline_loader_detail` namespace
 * (declarations in `loader_helpers.hpp`). The split matches the
 * natural schema boundaries (assets / tracks+clips / transitions);
 * each TU stays well under the 400-line debt threshold on its own.
 *
 * `LoadError` (throwing-by-value contract) and the primitive
 * parsers (as_rational, require, ...) still live in
 * loader_helpers.{hpp,cpp}. The `using namespace
 * me::timeline_loader_detail` here keeps call sites unqualified.
 */
#include "timeline/timeline_loader.hpp"
#include "timeline/loader_helpers.hpp"
#include "timeline/timeline_impl.hpp"

#include <nlohmann/json.hpp>

#include <new>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using json = nlohmann::json;
using namespace me::timeline_loader_detail;

}  // namespace

namespace me::timeline {

me_status_t load_json(std::string_view src, me_timeline** out, std::string* err) {
    if (!out) return ME_E_INVALID_ARG;
    *out = nullptr;

    try {
        json doc = json::parse(src);

        require(doc.value("schemaVersion", 0) == 1, ME_E_PARSE,
                "schemaVersion must be 1");

        auto* handle = new (std::nothrow) me_timeline{};
        if (!handle) return ME_E_OUT_OF_MEMORY;

        me::Timeline& tl = handle->tl;
        tl.frame_rate = as_rational(doc.at("frameRate"), "frameRate");
        const auto& res = doc.at("resolution");
        tl.width  = res.at("width").get<int>();
        tl.height = res.at("height").get<int>();
        require(tl.width > 0 && tl.height > 0, ME_E_PARSE,
                "resolution must be positive");

        parse_assets_into(doc, tl);

        /* --- Output composition selector -------------------------------- */
        const auto& output = doc.at("output");
        std::string target_comp = output.at("compositionId").get<std::string>();

        const auto& comps = doc.at("compositions");
        const json* comp = nullptr;
        for (const auto& c : comps) {
            if (c.at("id").get<std::string>() == target_comp) { comp = &c; break; }
        }
        require(comp != nullptr, ME_E_PARSE,
                "output.compositionId refers to unknown composition");

        /* --- Tracks + clips + transitions ------------------------------- */
        const auto& tracks = comp->at("tracks");
        require(!tracks.empty(), ME_E_PARSE,
                "composition.tracks must have at least one track");

        me_rational_t max_duration{0, 1};
        std::unordered_map<std::string, std::size_t> track_id_seen;

        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            parse_track_into(tracks[ti], ti, tl, max_duration, track_id_seen);
        }
        tl.duration = max_duration;

        *out = handle;
        return ME_OK;

    } catch (const LoadError& e) {
        if (err) *err = e.message;
        return e.status;
    } catch (const json::exception& e) {
        if (err) *err = std::string("json: ") + e.what();
        return ME_E_PARSE;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return ME_E_INTERNAL;
    }
}

}  // namespace me::timeline
