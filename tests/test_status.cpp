#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstring>
#include <set>
#include <string>
#include <string_view>

/* me_status_str is a static lookup; every enum value must produce a
 * non-empty, distinct, human-readable string. Regressions here usually
 * mean someone added a new ME_E_* value without wiring the switch. */
TEST_CASE("me_status_str covers every defined status enum value") {
    const me_status_t codes[] = {
        ME_OK, ME_E_INVALID_ARG, ME_E_OUT_OF_MEMORY, ME_E_IO,
        ME_E_PARSE, ME_E_DECODE, ME_E_ENCODE, ME_E_UNSUPPORTED,
        ME_E_CANCELLED, ME_E_NOT_FOUND, ME_E_INTERNAL,
    };
    std::set<std::string_view> seen;
    for (me_status_t s : codes) {
        const char* msg = me_status_str(s);
        REQUIRE(msg != nullptr);
        CHECK(std::strlen(msg) > 0);
        CHECK(seen.insert(msg).second);
    }
    /* Unknown integer values must still return a non-null sentinel rather
     * than crash or return NULL (API.md "never NULL"). */
    const char* unknown = me_status_str(static_cast<me_status_t>(12345));
    REQUIRE(unknown != nullptr);
    CHECK(std::strlen(unknown) > 0);
}

TEST_CASE("me_version returns a well-formed value") {
    me_version_t v = me_version();
    CHECK(v.major >= 0);
    CHECK(v.minor >= 0);
    CHECK(v.patch >= 0);
    REQUIRE(v.git_sha != nullptr);   /* empty string allowed, null forbidden */
}
