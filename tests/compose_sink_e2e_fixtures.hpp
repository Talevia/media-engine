/*
 * Shared fixtures + handle RAIIs for test_compose_sink_e2e_*.cpp.
 *
 * Extracted from the original 1136-line test_compose_sink_e2e.cpp
 * when the file was split by pipeline kind (debt-split-test-
 * compose-sink-e2e-cpp). Keeping the helpers here avoids drift
 * between split TUs when a handle contract changes.
 *
 * Consumers:
 *   - test_compose_sink_e2e.cpp           — video core + transitions.
 *   - test_compose_sink_e2e_audio.cpp     — audio / mixer / audio-only.
 *   - test_compose_sink_e2e_text.cpp      — text clip cases.
 *   - test_compose_sink_e2e_subtitle.cpp  — subtitle / fileUri cases.
 */
#pragma once

#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif
#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO
#define ME_TEST_FIXTURE_MP4_WITH_AUDIO ""
#endif

namespace me::tests::compose {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};
struct TimelineHandle {
    me_timeline_t* p = nullptr;
    ~TimelineHandle() { if (p) me_timeline_destroy(p); }
};
struct JobHandle {
    me_render_job_t* p = nullptr;
    ~JobHandle() { if (p) me_render_job_destroy(p); }
};

/* Build a 2-track timeline where both tracks use the shared
 * determinism fixture as their single clip. Loader creates
 * independent DemuxContexts per clip, so the compose loop gets
 * two genuinely independent decoder streams even though they
 * happen to read the same file. */
inline std::string two_track_timeline(const std::string& fixture_uri) {
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = fixture_uri});
    const int dur_num = 25, dur_den = 25;
    std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_v0","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}},
           "sourceRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c_v1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}},
           "sourceRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    return j;
}

}  // namespace me::tests::compose
