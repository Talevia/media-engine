#include "timeline/timeline_loader.hpp"
#include "timeline/timeline_impl.hpp"

#include <nlohmann/json.hpp>

#include <new>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace {

using json = nlohmann::json;

struct LoadError {
    me_status_t status;
    std::string message;
};

me_rational_t as_rational(const json& j, std::string_view field) {
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
        /* Map id → (uri, content_hash). content_hash: if JSON has
         * "contentHash": "sha256:<hex>", strip the prefix; any other
         * algorithm string is rejected because the engine only computes
         * sha256 (CLAUDE.md invariant: whitelist algo). */
        struct AssetEntry { std::string uri; std::string hash_hex; };
        std::unordered_map<std::string, AssetEntry> asset_map;
        if (doc.contains("assets")) {
            for (const auto& a : doc["assets"]) {
                std::string id  = a.at("id").get<std::string>();
                std::string uri = a.at("uri").get<std::string>();
                std::string hex;
                if (a.contains("contentHash") && a["contentHash"].is_string()) {
                    const std::string raw = a["contentHash"].get<std::string>();
                    constexpr std::string_view prefix{"sha256:"};
                    if (raw.size() > prefix.size() &&
                        raw.compare(0, prefix.size(), prefix) == 0) {
                        hex = raw.substr(prefix.size());
                        require(hex.size() == 64, ME_E_PARSE,
                                "asset[" + id + "].contentHash: sha256 hex must be 64 chars");
                        for (char c : hex) {
                            require((c >= '0' && c <= '9') ||
                                    (c >= 'a' && c <= 'f') ||
                                    (c >= 'A' && c <= 'F'),
                                    ME_E_PARSE,
                                    "asset[" + id + "].contentHash: non-hex char");
                        }
                        /* Normalize to lowercase for cache key stability. */
                        for (char& c : hex) {
                            if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
                        }
                    } else if (!raw.empty()) {
                        throw LoadError{ME_E_UNSUPPORTED,
                            "asset[" + id + "].contentHash: only \"sha256:\" prefix supported"};
                    }
                }
                asset_map.emplace(std::move(id), AssetEntry{std::move(uri), std::move(hex)});
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

        /* --- Phase-1 constraints: 1 video track, ≥1 video clip ---------- */
        const auto& tracks = comp->at("tracks");
        require(tracks.size() == 1, ME_E_UNSUPPORTED,
                "phase-1: exactly one track supported");
        const auto& track = tracks[0];
        require(track.at("kind").get<std::string>() == "video",
                ME_E_UNSUPPORTED, "phase-1: only video tracks supported");
        const auto& clips = track.at("clips");
        require(!clips.empty(), ME_E_PARSE, "track must have at least one clip");

        /* Walk clips in JSON order. Each clip's timeRange.start MUST equal the
         * running cumulative duration — phase-1 forbids gaps and overlaps so
         * the Exporter can emit a contiguous stream-copy without gap frames.
         * timeRange.duration MUST equal sourceRange.duration (no speed change
         * in phase-1). */
        me_rational_t running{0, 1};
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto& clip = clips[i];
            const std::string where = "clip[" + std::to_string(i) + "]";

            require(clip.at("type").get<std::string>() == "video",
                    ME_E_UNSUPPORTED, where + ": phase-1 only video clips");
            require(!clip.contains("effects") || clip["effects"].empty(),
                    ME_E_UNSUPPORTED, where + ": phase-1: clip.effects not supported");
            require(!clip.contains("transform"),
                    ME_E_UNSUPPORTED, where + ": phase-1: clip.transform not supported");

            const std::string asset_id = clip.at("assetId").get<std::string>();
            auto it = asset_map.find(asset_id);
            require(it != asset_map.end(), ME_E_PARSE,
                    where + ".assetId refers to unknown asset");

            const auto& tr = clip.at("timeRange");
            me_rational_t t_start = as_rational(tr.at("start"),    where + ".timeRange.start");
            me_rational_t t_dur   = as_rational(tr.at("duration"), where + ".timeRange.duration");

            const auto& sr = clip.at("sourceRange");
            me_rational_t s_start = as_rational(sr.at("start"),    where + ".sourceRange.start");
            me_rational_t s_dur   = as_rational(sr.at("duration"), where + ".sourceRange.duration");

            require(rational_eq(t_start, running), ME_E_UNSUPPORTED,
                    where + ".timeRange.start must equal cumulative prior clip duration "
                    "(phase-1: no gaps or overlaps)");
            require(rational_eq(t_dur, s_dur), ME_E_UNSUPPORTED,
                    where + ": phase-1: timeRange.duration must equal sourceRange.duration");
            require(t_dur.num > 0, ME_E_PARSE, where + ".timeRange.duration must be positive");
            require(s_start.num >= 0, ME_E_PARSE, where + ".sourceRange.start must be >= 0");

            me::Clip c;
            c.asset_uri      = it->second.uri;
            c.time_start     = t_start;
            c.time_duration  = t_dur;
            c.source_start   = s_start;
            c.content_hash   = it->second.hash_hex;
            tl.clips.push_back(std::move(c));

            /* running += t_dur in rational: a/b + c/d = (a*d + c*b) / (b*d).
             * No simplification; den stays bounded because phase-1 frames
             * rates produce modest denominators (30, 48000, 90000). */
            running = me_rational_t{
                running.num * t_dur.den + t_dur.num * running.den,
                running.den * t_dur.den
            };
        }
        tl.duration = running;

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
