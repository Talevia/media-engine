# Kotlin/Native cinterop ctest registration.
#
# `bindings/kotlin-native/` is a gradle-managed project; we don't
# build it through CMake. But we DO drive its smoke runner from
# ctest when both (a) ME_BUILD_TESTS is set and (b) a system gradle
# is on PATH — so a future cinterop drift (e.g. a public header
# adds a struct that media_engine.def's headerFilter doesn't pick
# up, or a function disappears from the public surface) trips the
# dev box's normal test loop instead of waiting for talevia to
# integrate.
#
# Scope of the smoke: gradle's `compileKotlinNative` task runs
# (a) `cinteropMediaEngineNative` — generates Kotlin bindings from
# the C public headers and (b) compiles `Main.kt` against those
# bindings. That covers the regression we actually care about:
# does the C surface still produce a valid K/N binding the example
# can compile against? The full executable link
# (`linkDebugExecutableNative` → `runDebugExecutableNative`) is
# blocked on a separable visibility design issue —
# `media_engine` sets `CXX_VISIBILITY_PRESET=hidden`, so the
# `me_*` C API symbols aren't exported when the engine is wrapped
# in a shared library. That needs an `ME_API` annotation pass on
# every public header (~34 functions across 9 headers); deferred
# to a follow-up bullet because it's a distinct ABI-scope decision.
#
# Cost: gradle bootstrap + Kotlin/Native compiler download is
# 2-5 minutes on first run; subsequent runs hit the global gradle
# + KN caches and complete in ~10-20 s. The TIMEOUT below is
# generous so first-time runs don't spuriously fail.
#
# Skipped silently when `gradle` is missing — no hard CMake
# requirement on the K/N toolchain. Hosts that don't ship K/N
# (CI image without JDK + gradle, embedded build environments)
# simply don't see the test.

if(NOT ME_BUILD_TESTS)
    return()
endif()

find_program(GRADLE_EXECUTABLE gradle)
if(NOT GRADLE_EXECUTABLE)
    message(STATUS "kotlin-native: gradle not found — "
                   "skipping kn_cinterop_smoke ctest")
    return()
endif()

# Run the example's compileKotlinNative gradle task. The
# example's build.gradle.kts walks `../../..` to find include/ +
# build/, so the gradle CWD must be the example dir.
add_test(NAME kn_cinterop_smoke
         COMMAND ${GRADLE_EXECUTABLE}
                 -p ${CMAKE_CURRENT_SOURCE_DIR}/example
                 -PmediaEngineRoot=${CMAKE_SOURCE_DIR}
                 --no-daemon
                 compileKotlinNative)
set_tests_properties(kn_cinterop_smoke PROPERTIES
    # First-run TIMEOUT: KN compiler download + gradle bootstrap
    # can take several minutes. After the first run, both are cached
    # and the test completes in seconds.
    TIMEOUT 600
    LABELS  "slow;cross-toolchain")
