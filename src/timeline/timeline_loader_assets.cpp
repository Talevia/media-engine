/* parse_assets_into — extract "assets" array into Timeline::assets.
 *
 * Scope-B slice of debt-split-timeline-loader-cpp. Previously lived
 * inline in timeline_loader.cpp. Handles:
 *   - Asset id + uri (required).
 *   - contentHash parsing: accepts only `"sha256:<64 hex chars>"`
 *     (normalized to lowercase for cache key stability); any other
 *     algorithm prefix → ME_E_UNSUPPORTED.
 *   - Optional per-asset `colorSpace` object.
 *
 * No implicit asset-id uniqueness check — the JSON schema allows
 * the loader to catch collisions via the unordered_map::emplace
 * failure, but we don't enforce it explicitly here to match the
 * original behavior (loader accepted duplicates by overwriting).
 * Tracked for a future cycle if it bites.
 */
#include "timeline/loader_helpers.hpp"
#include "timeline/timeline_impl.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

/* Parse the `"type"` JSON field (M11 ml-asset-schema). Backward-
 * compatible: absent / "media" → AssetKind::Media (existing JSON
 * shape, unchanged). Unknown values raise ME_E_UNSUPPORTED so a
 * typo doesn't silently degrade to Media. */
me::AssetKind parse_asset_kind(const json& a, const std::string& id) {
    if (!a.contains("type") || !a["type"].is_string()) {
        return me::AssetKind::Media;
    }
    const std::string t = a["type"].get<std::string>();
    if (t.empty() || t == "media")     return me::AssetKind::Media;
    if (t == "landmark")               return me::AssetKind::Landmark;
    if (t == "mask")                   return me::AssetKind::Mask;
    if (t == "keypoints")              return me::AssetKind::Keypoints;
    throw LoadError{ME_E_UNSUPPORTED,
        "asset[" + id + "].type: unknown value \"" + t +
        "\" (expected \"media\" / \"landmark\" / \"mask\" / \"keypoints\")"};
}

/* Parse the `"model"` sub-object — required for non-Media kinds.
 * M11 §136 mandates model_id / model_version / quantization. */
me::MlAssetMetadata parse_ml_metadata(const json& m, const std::string& id) {
    auto required_string = [&](const char* field) -> std::string {
        require(m.contains(field) && m[field].is_string(), ME_E_PARSE,
            "asset[" + id + "].model." + field + ": required string field");
        return m[field].get<std::string>();
    };
    me::MlAssetMetadata md;
    md.model_id      = required_string("id");
    md.model_version = required_string("version");
    md.quantization  = required_string("quantization");
    return md;
}

void parse_assets_into(const json& doc, me::Timeline& tl) {
    if (!doc.contains("assets")) return;

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
        std::optional<me::ColorSpace> cs;
        if (a.contains("colorSpace")) {
            cs = parse_color_space(a["colorSpace"], "asset[" + id + "]");
        }

        const me::AssetKind kind = parse_asset_kind(a, id);
        std::optional<me::MlAssetMetadata> ml;
        if (kind != me::AssetKind::Media) {
            require(a.contains("model") && a["model"].is_object(), ME_E_PARSE,
                "asset[" + id + "].model: required object for non-media asset kind");
            ml = parse_ml_metadata(a["model"], id);
        } else {
            require(!a.contains("model"), ME_E_PARSE,
                "asset[" + id + "].model: only valid when type is non-media");
        }

        me::Asset asset{
            std::move(uri),
            std::move(hex),
            std::move(cs),
            kind,
            std::move(ml),
        };
        tl.assets.emplace(std::move(id), std::move(asset));
    }
}

}  // namespace me::timeline_loader_detail
