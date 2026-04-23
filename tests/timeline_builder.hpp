/*
 * TimelineBuilder — header-only helper for producing minimal valid
 * timeline JSON in tests.
 *
 * Rationale: every test suite that calls `me_timeline_load_json` used to
 * hand-roll a JSON string with the full `schemaVersion / frameRate /
 * resolution / colorSpace / assets / compositions / output` hierarchy,
 * then mutate it with `find + replace` for negative cases. Any schema
 * field rename rippled through N test files in lock-step. The builder
 * collapses the common shape into one place and offers fluent setters
 * for the fields tests actually vary (schema version, clip count,
 * asset contentHash, clip effects / transform injection).
 *
 * Usage:
 *   auto json = tb::minimal_video_clip().build();                // default single clip
 *   auto j2   = tb::minimal_video_clip("file:///tmp/x.mp4").build();
 *   auto bad  = tb::minimal_video_clip().schema_version(2).build();
 *   auto fx   = tb::minimal_video_clip()
 *                  .with_clip_extra(R"("effects":[{"kind":"blur"}],)")
 *                  .build();
 *   auto two  = tb::TimelineBuilder()
 *                  .add_asset({.id="a1", .uri="file:///tmp/a.mp4"})
 *                  .add_clip({.clip_id="c1", .asset_id="a1"})
 *                  .add_clip({.clip_id="c2", .asset_id="a1",
 *                             .time_start_num=60, .time_start_den=30})
 *                  .build();
 *
 * Kept header-only because tests are the only consumers and the builder
 * has no state beyond what each instance holds. No JSON library
 * dependency — it just concatenates strings; the engine's loader is
 * what parses.
 */
#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace me::tests::tb {

struct AssetSpec {
    std::string id           = "a1";
    std::string kind         = "video";
    std::string uri          = "file:///tmp/input.mp4";
    /* Empty = omit the `contentHash` field entirely. Non-empty should be
     * the full `sha256:<64-hex>` form; the loader normalizes. */
    std::string content_hash;
    /* Empty = omit the `colorSpace` field entirely. Non-empty should be
     * a full JSON object body e.g. `{"primaries":"bt709","range":"limited"}`. */
    std::string color_space_json;
};

struct ClipSpec {
    std::string clip_id          = "c1";
    std::string type             = "video";
    std::string asset_id         = "a1";
    int         time_start_num   = 0;
    int         time_start_den   = 30;
    int         time_dur_num     = 60;
    int         time_dur_den     = 30;
    int         source_start_num = 0;
    int         source_start_den = 30;
    int         source_dur_num   = 60;
    int         source_dur_den   = 30;
    /* Raw JSON fragment injected between `assetId` and `timeRange` fields.
     * Must be a valid JSON tail ending in a comma, e.g.:
     *     R"("effects":[{"kind":"blur"}],)"
     * Used for negative cases that exercise rejection of disallowed
     * fields (effects, transform) without reshaping the builder API. */
    std::string extra;
};

class TimelineBuilder {
public:
    TimelineBuilder& schema_version(int v) { schema_version_ = v; return *this; }
    TimelineBuilder& frame_rate(int num, int den) { fr_num_ = num; fr_den_ = den; return *this; }
    TimelineBuilder& resolution(int w, int h) { width_ = w; height_ = h; return *this; }
    TimelineBuilder& add_asset(AssetSpec a) { assets_.push_back(std::move(a)); return *this; }
    TimelineBuilder& add_clip(ClipSpec c)   { clips_.push_back(std::move(c));  return *this; }

    /* Convenience: injects `extra` into the first clip. Useful when
     * minimal_video_clip() is chained and only one clip exists. */
    TimelineBuilder& with_clip_extra(std::string extra) {
        if (clips_.empty()) {
            clips_.push_back(ClipSpec{});
        }
        clips_.front().extra = std::move(extra);
        return *this;
    }

    std::string build() const {
        std::ostringstream os;
        os << "{\n";
        os << "  \"schemaVersion\": " << schema_version_ << ",\n";
        os << "  \"frameRate\":  {\"num\":" << fr_num_ << ",\"den\":" << fr_den_ << "},\n";
        os << "  \"resolution\": {\"width\":" << width_ << ",\"height\":" << height_ << "},\n";
        os << "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
              "\"matrix\":\"bt709\",\"range\":\"limited\"},\n";
        os << "  \"assets\": [";
        for (size_t i = 0; i < assets_.size(); ++i) {
            const auto& a = assets_[i];
            os << (i ? "," : "") << "\n    {\"id\":\"" << a.id
               << "\",\"kind\":\"" << a.kind
               << "\",\"uri\":\"" << a.uri << "\"";
            if (!a.content_hash.empty()) {
                os << ",\"contentHash\":\"" << a.content_hash << "\"";
            }
            if (!a.color_space_json.empty()) {
                os << ",\"colorSpace\":" << a.color_space_json;
            }
            os << "}";
        }
        os << "\n  ],\n";
        os << "  \"compositions\": [\n";
        os << "    {\"id\":\"main\",\"tracks\":[\n";
        os << "      {\"id\":\"v0\",\"kind\":\"video\",\"clips\":[";
        for (size_t i = 0; i < clips_.size(); ++i) {
            const auto& c = clips_[i];
            os << (i ? "," : "") << "\n        {"
               << "\"type\":\"" << c.type << "\","
               << "\"id\":\"" << c.clip_id << "\","
               << "\"assetId\":\"" << c.asset_id << "\","
               << c.extra
               << "\"timeRange\":{\"start\":{\"num\":" << c.time_start_num
                  << ",\"den\":" << c.time_start_den << "},"
                  << "\"duration\":{\"num\":" << c.time_dur_num
                  << ",\"den\":" << c.time_dur_den << "}},"
               << "\"sourceRange\":{\"start\":{\"num\":" << c.source_start_num
                  << ",\"den\":" << c.source_start_den << "},"
                  << "\"duration\":{\"num\":" << c.source_dur_num
                  << ",\"den\":" << c.source_dur_den << "}}"
               << "}";
        }
        os << "\n      ]}\n";
        os << "    ]}\n";
        os << "  ],\n";
        os << "  \"output\": {\"compositionId\":\"main\"}\n";
        os << "}\n";
        return os.str();
    }

private:
    int schema_version_ = 1;
    int fr_num_ = 30, fr_den_ = 1;
    int width_ = 1920, height_ = 1080;
    std::vector<AssetSpec> assets_;
    std::vector<ClipSpec>  clips_;
};

/* Canonical "minimal valid" timeline: one asset, one clip that consumes
 * the whole asset's duration starting at t=0. */
inline TimelineBuilder minimal_video_clip(const std::string& uri = "file:///tmp/input.mp4") {
    TimelineBuilder b;
    b.add_asset(AssetSpec{.uri = uri});
    b.add_clip(ClipSpec{});
    return b;
}

}  // namespace me::tests::tb
