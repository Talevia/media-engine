# Kotlin/Native cinterop ctest registration.
#
# `bindings/kotlin-native/` is a gradle-managed project; we don't
# build it through CMake. But we DO drive its smoke runner from
# ctest when both (a) ME_BUILD_TESTS is set and (b) a system gradle
# is on PATH — so a future cinterop drift (e.g. a public header
# adds a struct that media_engine.def's headerFilter doesn't pick
# up, or a symbol disappears from the link line) trips the dev
# box's normal test loop instead of waiting for talevia to
# integrate.
#
# A SHARED library `media_engine_kn` mirrors the `bindings/jni`
# pattern: it wraps the static `media_engine` archive and lets
# CMake handle the transitive deps (FFmpeg / OCIO / libass /
# Skia / SoundTouch / bgfx, whichever are configured). The K/N
# example links against this dylib, so the gradle build never
# has to enumerate engine-internal deps — adding ME_WITH_OCIO=ON
# (etc.) downstream doesn't break the K/N link line. Public
# `me_*` symbols are re-exported via the `ME_API` macro (added to
# every public header in the cycle that landed
# `debt-bindings-kn-link-visibility`); engine-internal C++
# symbols stay hidden per `media_engine`'s
# CXX_VISIBILITY_PRESET=hidden.
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

# media_engine_kn — SHARED dylib wrapping libmedia_engine.a so the
# Kotlin/Native example sees a single library with all engine
# transitive deps embedded. Built unconditionally when this
# CMakeLists.txt is included so `cmake --build build --target
# media_engine_kn` produces the dylib gradle expects, regardless
# of whether ME_BUILD_TESTS is on.
add_library(media_engine_kn SHARED kn_link_stub.c)
set_target_properties(media_engine_kn PROPERTIES
    OUTPUT_NAME media_engine_kn
    POSITION_INDEPENDENT_CODE ON)
# macOS dyld needs the dylib's install_name to be findable at
# runtime. CMake's default is `@rpath/libmedia_engine_kn.dylib`,
# which would force every consumer to set LC_RPATH; the K/N
# executable doesn't natively do that. Setting install_name to
# the dylib's absolute build-tree path lets the kexe find it
# without RPATH wiring. This is correct for the dev-box smoke
# test only — a real ship would either re-link the dylib
# in-place at deploy time or wire @rpath properly. */
if(APPLE)
    set_target_properties(media_engine_kn PROPERTIES
        INSTALL_NAME_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        BUILD_WITH_INSTALL_NAME_DIR ON)
endif()
# Whole-archive: libmedia_engine.a is otherwise stripped of any
# .o whose symbols nothing in kn_link_stub.c references — which
# is most of the engine. -force_load preserves every member,
# matching what the JNI binding does for the same reason.
# ME_API on the public-header declarations re-exports each
# `me_*` C function with default visibility; engine-internal
# C++ symbols stay hidden per the engine's
# CXX_VISIBILITY_PRESET=hidden.
if(APPLE)
    target_link_libraries(media_engine_kn PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:media_engine>"
        media_engine)
else()
    target_link_libraries(media_engine_kn PRIVATE
        "-Wl,--whole-archive"
        media_engine
        "-Wl,--no-whole-archive")
endif()

if(NOT ME_BUILD_TESTS)
    return()
endif()

find_program(GRADLE_EXECUTABLE gradle)
if(NOT GRADLE_EXECUTABLE)
    message(STATUS "kotlin-native: gradle not found — "
                   "skipping kn_cinterop_smoke ctest")
    return()
endif()

# Run the example's runDebugExecutableNative gradle task. The
# example's build.gradle.kts walks `../../..` to find include/ +
# build/, so the gradle CWD must be the example dir. The
# -PmediaEngineKnLib pin steers the linker at the SHARED
# dylib's location which add_library(SHARED) places under
# `${CMAKE_CURRENT_BINARY_DIR}` (= build/bindings/kotlin-native/).
add_test(NAME kn_cinterop_smoke
         COMMAND ${GRADLE_EXECUTABLE}
                 -p ${CMAKE_CURRENT_SOURCE_DIR}/example
                 -PmediaEngineRoot=${CMAKE_SOURCE_DIR}
                 -PmediaEngineKnLib=$<TARGET_FILE_DIR:media_engine_kn>
                 --no-daemon
                 runDebugExecutableNative)
set_tests_properties(kn_cinterop_smoke PROPERTIES
    # First-run TIMEOUT: KN compiler download + gradle bootstrap
    # can take several minutes. After the first run, both are cached
    # and the test completes in seconds.
    TIMEOUT 600
    LABELS  "slow;cross-toolchain")
