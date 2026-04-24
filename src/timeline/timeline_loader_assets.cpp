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
        tl.assets.emplace(std::move(id),
                           me::Asset{std::move(uri), std::move(hex),
                                     std::move(cs)});
    }
}

}  // namespace me::timeline_loader_detail
