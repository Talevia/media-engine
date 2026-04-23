#include <doctest/doctest.h>

#include <media_engine.h>

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

TEST_CASE("engine_create(NULL config) succeeds and returns a handle") {
    me_engine_t* eng = nullptr;
    me_status_t s = me_engine_create(nullptr, &eng);
    CHECK(s == ME_OK);
    REQUIRE(eng != nullptr);
    me_engine_destroy(eng);
}

TEST_CASE("engine_create with NULL out rejects cleanly") {
    /* API.md: invalid arguments must not crash, must return
     * ME_E_INVALID_ARG, and must not leak an engine. */
    me_status_t s = me_engine_create(nullptr, nullptr);
    CHECK(s == ME_E_INVALID_ARG);
}

TEST_CASE("engine_destroy(NULL) is a no-op") {
    /* Symmetric with free(NULL); callers rely on this for simple cleanup. */
    me_engine_destroy(nullptr);   /* must not crash */
}

TEST_CASE("engine_last_error on fresh engine is the empty string") {
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);
    const char* err = me_engine_last_error(eng);
    REQUIRE(err != nullptr);
    CHECK(std::strcmp(err, "") == 0);
    me_engine_destroy(eng);
}

TEST_CASE("engine_last_error(NULL) returns a non-null sentinel") {
    const char* err = me_engine_last_error(nullptr);
    REQUIRE(err != nullptr);
    CHECK(std::strcmp(err, "") == 0);
}

TEST_CASE("explicit config propagates without error") {
    me_engine_config_t cfg{};
    cfg.num_worker_threads = 2;
    cfg.log_level          = ME_LOG_WARN;
    cfg.cache_dir          = nullptr;
    cfg.memory_cache_bytes = 0;

    me_engine_t* eng = nullptr;
    CHECK(me_engine_create(&cfg, &eng) == ME_OK);
    REQUIRE(eng != nullptr);
    me_engine_destroy(eng);
}

/* --- Thread-local last-error contract (docs/API.md §threading) --------
 * "me_engine_last_error is thread-local per engine: thread A's error does
 * not clobber thread B's view." The test triggers errors on two threads
 * against the same engine and verifies each thread sees only its own. */
TEST_CASE("last_error is thread-local per engine") {
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    /* Provoke an error on thread T1: invalid JSON → ME_E_PARSE with
     * a message containing "json" from nlohmann's diagnostic. */
    const std::string_view bad_json_t1 = "{ not json }";

    std::atomic<int> barrier{0};
    std::string t1_seen;
    std::string t2_seen;

    std::thread t1([&]() {
        me_timeline_t* tl = nullptr;
        me_timeline_load_json(eng, bad_json_t1.data(), bad_json_t1.size(), &tl);
        ++barrier;
        while (barrier.load() < 2) { /* spin until t2 also loaded */ }
        /* After t2 has also set an error on the same engine, read ours. */
        t1_seen = me_engine_last_error(eng);
    });

    std::thread t2([&]() {
        const std::string_view bad_schema_t2 = R"({"schemaVersion":999})";
        me_timeline_t* tl = nullptr;
        me_timeline_load_json(eng, bad_schema_t2.data(), bad_schema_t2.size(), &tl);
        ++barrier;
        while (barrier.load() < 2) { /* spin */ }
        t2_seen = me_engine_last_error(eng);
    });

    t1.join();
    t2.join();

    REQUIRE(!t1_seen.empty());
    REQUIRE(!t2_seen.empty());
    /* Each thread's view is its own error, not the peer's. */
    CHECK(t1_seen.find("json") != std::string::npos);
    CHECK(t2_seen.find("schemaVersion") != std::string::npos);
    /* The main thread, which never invoked an API on this engine, sees
     * its own (empty) slot. */
    CHECK(std::strcmp(me_engine_last_error(eng), "") == 0);

    me_engine_destroy(eng);
}

TEST_CASE("clear_error on success path doesn't leak prior errors on this thread") {
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);

    /* Step 1: induce an error. */
    const char* bad = "{not json}";
    me_timeline_t* tl = nullptr;
    CHECK(me_timeline_load_json(eng, bad, std::strlen(bad), &tl) == ME_E_PARSE);
    CHECK(std::string_view{me_engine_last_error(eng)}.size() > 0);

    /* Step 2: a subsequent successful call on the same engine clears the
     * slot via me::detail::clear_error on entry (see each entry point). */
    const char* ok_json = R"({
      "schemaVersion": 1,
      "frameRate":  {"num": 30, "den": 1},
      "resolution": {"width": 1920, "height": 1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets":[{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
        {"type":"video","id":"c1","assetId":"a1",
         "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
         "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
      ]}]}],
      "output":{"compositionId":"main"}
    })";
    me_timeline_t* ok_tl = nullptr;
    REQUIRE(me_timeline_load_json(eng, ok_json, std::strlen(ok_json), &ok_tl) == ME_OK);
    CHECK(std::strcmp(me_engine_last_error(eng), "") == 0);

    me_timeline_destroy(ok_tl);
    me_engine_destroy(eng);
}
