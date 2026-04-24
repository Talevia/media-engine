/*
 * Shared fixtures + helpers for test_timeline_schema_*.cpp.
 *
 * Extracted from the original 1535-line test_timeline_schema.cpp
 * when the file was split by schema section. Keeping the helpers
 * here (instead of duplicating them into each split file) avoids
 * drift when a fixture shape changes.
 *
 * Consumers:
 *   - test_timeline_schema.cpp              — core schema cases.
 *   - test_timeline_schema_audio.cpp        — audio / gainDb.
 *   - test_timeline_schema_clip_attributes.cpp
 *                                           — effects, transform,
 *                                             asset colorSpace,
 *                                             text / subtitle
 *                                             clip params.
 *   - test_timeline_schema_transitions.cpp  — transitions.
 */
#pragma once

#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"
#include "timeline/timeline_impl.hpp"

#include <string>

namespace tb = me::tests::tb;

namespace me::tests::schema {

inline me_status_t load(me_engine_t* eng, const std::string& json,
                         me_timeline_t** out) {
    return me_timeline_load_json(eng, json.data(), json.size(), out);
}

struct EngineFixture {
    me_engine_t* eng = nullptr;
    EngineFixture()  { REQUIRE(me_engine_create(nullptr, &eng) == ME_OK); }
    ~EngineFixture() { me_engine_destroy(eng); }
};

}  // namespace me::tests::schema
