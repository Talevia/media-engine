#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstring>

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
