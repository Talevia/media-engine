#include "timeline/timeline_loader.hpp"
#include "timeline/timeline_impl.hpp"

#include <nlohmann/json.hpp>

#include <new>
#include <sstream>
#include <unordered_map>

namespace {

using json = nlohmann::json;

struct LoadError {
    me_status_t status;
    std::string message;
};

me_rational_t as_rational(const json& j, const char* field) {
    if (!j.is_object() || !j.contains("num") || !j.contains("den")) {
        throw LoadError{ME_E_PARSE, std::string(field) + ": expected {num,den}"};
    }
    int64_t num = j["num"].get<int64_t>();
    int64_t den = j["den"].get<int64_t>();
    if (den <= 0) throw LoadError{ME_E_PARSE, std::string(field) + ".den must be > 0"};
    return me_rational_t{num, den};
}

void require(bool cond, me_status_t s, std::string msg) {
    if (!cond) throw LoadError{s, std::move(msg)};
}

bool rational_eq(me_rational_t a, me_rational_t b) {
    /* a/b == c/d  <=>  a*d == c*b */
    return a.num * b.den == b.num * a.den;
}

}  // namespace

namespace me::timeline {

me_status_t load_json(std::string_view src, me_timeline** out, std::string* err) {
    if (!out) return ME_E_INVALID_ARG;
    *out = nullptr;

    try {
        json doc = json::parse(src);

        require(doc.value("schemaVersion", 0) == 1, ME_E_PARSE,
                "schemaVersion must be 1");

        /* --- Top-level -------------------------------------------------- */
        auto* handle = new (std::nothrow) me_timeline{};
        if (!handle) return ME_E_OUT_OF_MEMORY;

        me::Timeline& tl = handle->tl;
        tl.frame_rate = as_rational(doc.at("frameRate"), "frameRate");
        const auto& res = doc.at("resolution");
        tl.width  = res.at("width").get<int>();
        tl.height = res.at("height").get<int>();
        require(tl.width > 0 && tl.height > 0, ME_E_PARSE,
                "resolution must be positive");

        /* --- Assets (index by id) --------------------------------------- */
        std::unordered_map<std::string, std::string> asset_uri;
        if (doc.contains("assets")) {
            for (const auto& a : doc["assets"]) {
                std::string id  = a.at("id").get<std::string>();
                std::string uri = a.at("uri").get<std::string>();
                asset_uri.emplace(std::move(id), std::move(uri));
            }
        }

        /* --- Output composition selector -------------------------------- */
        const auto& output = doc.at("output");
        std::string target_comp = output.at("compositionId").get<std::string>();

        /* --- Find target composition ----------------------------------- */
        const auto& comps = doc.at("compositions");
        const json* comp = nullptr;
        for (const auto& c : comps) {
            if (c.at("id").get<std::string>() == target_comp) { comp = &c; break; }
        }
        require(comp != nullptr, ME_E_PARSE,
                "output.compositionId refers to unknown composition");

        /* --- Phase-1 constraints: 1 video track, 1 video clip ----------- */
        const auto& tracks = comp->at("tracks");
        require(tracks.size() == 1, ME_E_UNSUPPORTED,
                "phase-1: exactly one track supported");
        const auto& track = tracks[0];
        require(track.at("kind").get<std::string>() == "video",
                ME_E_UNSUPPORTED, "phase-1: only video tracks supported");
        const auto& clips = track.at("clips");
        require(clips.size() == 1, ME_E_UNSUPPORTED,
                "phase-1: exactly one clip supported");

        const auto& clip = clips[0];
        require(clip.at("type").get<std::string>() == "video",
                ME_E_UNSUPPORTED, "phase-1: only video clips supported");

        /* No effects / transforms in phase-1 passthrough. */
        require(!clip.contains("effects") || clip["effects"].empty(),
                ME_E_UNSUPPORTED, "phase-1: clip.effects not supported");
        require(!clip.contains("transform"),
                ME_E_UNSUPPORTED, "phase-1: clip.transform not supported");

        std::string asset_id = clip.at("assetId").get<std::string>();
        auto it = asset_uri.find(asset_id);
        require(it != asset_uri.end(), ME_E_PARSE,
                "clip.assetId refers to unknown asset");

        const auto& tr = clip.at("timeRange");
        me_rational_t t_start = as_rational(tr.at("start"),    "clip.timeRange.start");
        me_rational_t t_dur   = as_rational(tr.at("duration"), "clip.timeRange.duration");

        const auto& sr = clip.at("sourceRange");
        me_rational_t s_start = as_rational(sr.at("start"),    "clip.sourceRange.start");
        me_rational_t s_dur   = as_rational(sr.at("duration"), "clip.sourceRange.duration");

        require(t_start.num == 0, ME_E_UNSUPPORTED,
                "phase-1: clip.timeRange.start must be zero");
        require(rational_eq(t_dur, s_dur), ME_E_UNSUPPORTED,
                "phase-1: timeRange.duration must equal sourceRange.duration");
        require(s_start.num == 0, ME_E_UNSUPPORTED,
                "phase-1: clip.sourceRange.start must be zero");

        me::Clip c;
        c.asset_uri      = it->second;
        c.time_start     = t_start;
        c.time_duration  = t_dur;
        c.source_start   = s_start;
        tl.clips.push_back(std::move(c));
        tl.duration = t_dur;

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
