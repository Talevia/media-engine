# Kotlin/Native cinterop ctest registration.
#
# `bindings/kotlin-native/` is a gradle-managed project; we don't
# build it through CMake. But we DO drive its smoke runner from
# ctest when both (a) ME_BUILD_TESTS is set and (b) a system gradle
# is on PATH — so a future cinterop drift (e.g. a public header
# adds a struct that media_engine.def's headerFilter doesn't pick
# up, or a symbol disappears from the link line) trips the dev
# box's normal test loop instead of waiting for talevia to integrate.
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

# Version gate: this example pins kotlin("multiplatform") 2.0.20,
# which depends on Gradle APIs removed in Gradle 9 (specifically
# DefaultArtifactPublicationSet). Skip the test when the system
# gradle is too new — the cleaner alternative is bumping the
# Kotlin pin + porting Main.kt to the new K/N cinterop opaque-
# handle convention, both deferred to a future cycle.
execute_process(COMMAND ${GRADLE_EXECUTABLE} --version
                OUTPUT_VARIABLE _gradle_version_text
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "Gradle ([0-9]+)\\." _ "${_gradle_version_text}")
set(_gradle_major "${CMAKE_MATCH_1}")
if(_gradle_major AND _gradle_major GREATER_EQUAL 9)
    message(STATUS "kotlin-native: gradle ${_gradle_major}.x is incompatible "
                   "with the Kotlin 2.0.20 multiplatform plugin pinned by "
                   "this example — skipping kn_cinterop_smoke ctest")
    return()
endif()

# Run the example's runDebugExecutableNative gradle task. The
# example's build.gradle.kts walks `../../..` to find include/ +
# build/, so the gradle CWD must be the example dir.
add_test(NAME kn_cinterop_smoke
         COMMAND ${GRADLE_EXECUTABLE}
                 -p ${CMAKE_CURRENT_SOURCE_DIR}/example
                 -PmediaEngineRoot=${CMAKE_SOURCE_DIR}
                 --no-daemon
                 runDebugExecutableNative)
set_tests_properties(kn_cinterop_smoke PROPERTIES
    # First-run TIMEOUT: KN compiler download + gradle bootstrap
    # can take several minutes. After the first run, both are cached
    # and the test completes in seconds.
    TIMEOUT 600
    LABELS  "slow;cross-toolchain")
