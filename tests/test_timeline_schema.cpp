#include <doctest/doctest.h>

#include <media_engine.h>

#include <string>
#include <string_view>

namespace {

/* Minimal valid single-clip single-track timeline. Tests mutate it to
 * exercise the loader's rejection paths. */
const char* const kValidTimeline = R"({
  "schemaVersion": 1,
  "frameRate":  {"num": 30, "den": 1},
  "resolution": {"width": 1920, "height": 1080},
  "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets": [
    {"id":"a1","kind":"video","uri":"file:///tmp/input.mp4"}
  ],
  "compositions": [
    {
      "id":"main",
      "tracks":[
        {"id":"v0","kind":"video","clips":[
          {
            "type":"video","id":"c1","assetId":"a1",
            "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
            "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}
          }
        ]}
      ]
    }
  ],
  "output":{"compositionId":"main"}
})";

me_status_t load(me_engine_t* eng, std::string_view json, me_timeline_t** out) {
    return me_timeline_load_json(eng, json.data(), json.size(), out);
}

struct EngineFixture {
    me_engine_t* eng = nullptr;
    EngineFixture()  { REQUIRE(me_engine_create(nullptr, &eng) == ME_OK); }
    ~EngineFixture() { me_engine_destroy(eng); }
};

}  // namespace

TEST_CASE("valid single-clip timeline loads") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, kValidTimeline, &tl) == ME_OK);
    REQUIRE(tl != nullptr);

    int w = 0, h = 0;
    me_timeline_resolution(tl, &w, &h);
    CHECK(w == 1920);
    CHECK(h == 1080);

    me_rational_t fr = me_timeline_frame_rate(tl);
    CHECK(fr.num == 30);
    CHECK(fr.den == 1);

    me_timeline_destroy(tl);
}

TEST_CASE("schemaVersion != 1 is rejected as ME_E_PARSE") {
    EngineFixture f;
    std::string json{kValidTimeline};
    auto pos = json.find("\"schemaVersion\": 1");
    REQUIRE(pos != std::string::npos);
    json.replace(pos, std::string_view("\"schemaVersion\": 1").size(),
                 "\"schemaVersion\": 2");
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, json, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("malformed JSON is rejected as ME_E_PARSE") {
    EngineFixture f;
    std::string_view bad = R"({ not valid json )";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, bad, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("phase-1 rejects multi-clip timelines") {
    EngineFixture f;
    std::string json{kValidTimeline};
    /* Duplicate the clip: split after the single existing clip object. */
    const std::string_view clip_block = R"({
            "type":"video","id":"c1","assetId":"a1",
            "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
            "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}
          })";
    auto pos = json.find(clip_block);
    REQUIRE(pos != std::string::npos);
    std::string second_clip = std::string(clip_block);
    /* Give the second clip a distinct id so the loader reaches the "exactly
     * one clip" check rather than failing earlier on duplicate ids. */
    auto idpos = second_clip.find("\"id\":\"c1\"");
    REQUIRE(idpos != std::string::npos);
    second_clip.replace(idpos, std::string_view("\"id\":\"c1\"").size(),
                         "\"id\":\"c2\"");
    json.insert(pos + clip_block.size(), "," + second_clip);

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, json, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
}

TEST_CASE("phase-1 rejects clip.effects") {
    EngineFixture f;
    std::string json{kValidTimeline};
    auto pos = json.find("\"assetId\":\"a1\",");
    REQUIRE(pos != std::string::npos);
    json.insert(pos, R"("effects":[{"kind":"blur","params":{"radius":{"num":1,"den":1}}}],)");
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, json, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
}

TEST_CASE("load_json(NULL engine) returns ME_E_INVALID_ARG") {
    me_timeline_t* tl = nullptr;
    std::string_view s = kValidTimeline;
    CHECK(me_timeline_load_json(nullptr, s.data(), s.size(), &tl) == ME_E_INVALID_ARG);
    CHECK(tl == nullptr);
}

TEST_CASE("load_json populates last_error on rejection") {
    EngineFixture f;
    std::string_view bad = R"({"schemaVersion":2})";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, bad, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("schemaVersion") != std::string_view::npos);
}
