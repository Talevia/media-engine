# JNI glue — opt-in via root `-DME_BUILD_JNI=ON`.
#
# Produces `libmedia_engine_jni.{dylib,so,dll}` that Java hosts pick
# up via `System.loadLibrary("media_engine_jni")`. The JNI lib
# transitively depends on the main engine; hosts must keep both
# libraries reachable from `java.library.path` (or bundle both into
# the JAR resources tree they extract from at startup).
#
# Compilation of `MediaEngine.java` into a classfile is NOT wired into
# CMake — keep the C++ build toolchain-minimal. Hosts use their own
# build system (Maven / Gradle / Bazel) for the Java side. For local
# smoke runs:
#   javac -d build/classes bindings/jni/src/io/mediaengine/MediaEngine.java
#   java -Djava.library.path=build -cp build/classes \
#        io.mediaengine.MediaEngine <timeline.json> <out.mp4>

find_package(JNI REQUIRED)

add_library(media_engine_jni SHARED me_jni.cpp)

target_include_directories(media_engine_jni PRIVATE
    ${JNI_INCLUDE_DIRS}
)

target_link_libraries(media_engine_jni PRIVATE
    media_engine
)

set_target_properties(media_engine_jni PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# JNI headers sometimes trip `-Wpedantic` / `-Wmissing-prototypes`;
# relax at the target level without weakening project-wide -Werror.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(media_engine_jni PRIVATE -Wno-pedantic)
endif()

# --- Optional ctest: load + smoke-call the JNI bridge from a JVM ---------
# Without this, libmedia_engine_jni builds clean but no test ever
# resolves its symbols against a JNIEnv. A signature drift (renamed
# JNIEXPORT entrypoint, changed Java native method shape) goes
# undetected until talevia integrates and trips UnsatisfiedLinkError.
#
# Gated on (a) ME_BUILD_TESTS and (b) a usable Java toolchain — without
# either we emit a status note and skip, so the build stays clean on
# host machines that don't have a JDK installed.
if(ME_BUILD_TESTS)
    find_package(Java COMPONENTS Development Runtime QUIET)
    if(Java_JAVAC_EXECUTABLE AND Java_JAVA_EXECUTABLE)
        set(_me_jni_java_src
            "${CMAKE_CURRENT_SOURCE_DIR}/src/io/mediaengine/MediaEngine.java")
        set(_me_jni_run_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/Run.java")
        set(_me_jni_verify_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/VersionAssert.java")
        set(_me_jni_thumb_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/Thumbnail.java")
        set(_me_jni_cancel_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/Cancel.java")
        set(_me_jni_status_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/LastStatusAssert.java")
        set(_me_jni_throw_src
            "${CMAKE_CURRENT_SOURCE_DIR}/example/src/io/mediaengine/example/ProgressThrow.java")
        set(_me_jni_classdir "${CMAKE_CURRENT_BINARY_DIR}/classes")
        set(_me_jni_classfile
            "${_me_jni_classdir}/io/mediaengine/MediaEngine.class")
        set(_me_jni_run_classfile
            "${_me_jni_classdir}/io/mediaengine/example/Run.class")
        set(_me_jni_verify_classfile
            "${_me_jni_classdir}/io/mediaengine/example/VersionAssert.class")
        set(_me_jni_thumb_classfile
            "${_me_jni_classdir}/io/mediaengine/example/Thumbnail.class")
        set(_me_jni_cancel_classfile
            "${_me_jni_classdir}/io/mediaengine/example/Cancel.class")
        set(_me_jni_status_classfile
            "${_me_jni_classdir}/io/mediaengine/example/LastStatusAssert.class")
        set(_me_jni_throw_classfile
            "${_me_jni_classdir}/io/mediaengine/example/ProgressThrow.class")

        # Compile MediaEngine.java + the example runners together; the
        # runners depend on the wrapper class. Re-runs only when any
        # .java in the input list changes. Keep the classes/ tree
        # under the binary dir so a clean build wipes it.
        add_custom_command(
            OUTPUT  "${_me_jni_classfile}"
                    "${_me_jni_run_classfile}"
                    "${_me_jni_verify_classfile}"
                    "${_me_jni_thumb_classfile}"
                    "${_me_jni_cancel_classfile}"
                    "${_me_jni_status_classfile}"
                    "${_me_jni_throw_classfile}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_me_jni_classdir}"
            COMMAND ${Java_JAVAC_EXECUTABLE}
                    -d "${_me_jni_classdir}"
                    "${_me_jni_java_src}"
                    "${_me_jni_run_src}"
                    "${_me_jni_verify_src}"
                    "${_me_jni_thumb_src}"
                    "${_me_jni_cancel_src}"
                    "${_me_jni_status_src}"
                    "${_me_jni_throw_src}"
            DEPENDS "${_me_jni_java_src}"
                    "${_me_jni_run_src}"
                    "${_me_jni_verify_src}"
                    "${_me_jni_thumb_src}"
                    "${_me_jni_cancel_src}"
                    "${_me_jni_status_src}"
                    "${_me_jni_throw_src}"
            COMMENT "javac MediaEngine.java + example runners -> ${_me_jni_classdir}")

        add_custom_target(media_engine_jni_classes
            DEPENDS "${_me_jni_classfile}"
                    "${_me_jni_run_classfile}"
                    "${_me_jni_verify_classfile}"
                    "${_me_jni_thumb_classfile}"
                    "${_me_jni_cancel_classfile}"
                    "${_me_jni_status_classfile}"
                    "${_me_jni_throw_classfile}")
        add_dependencies(media_engine_jni_classes media_engine_jni)

        # jni_load_smoke: no-args MediaEngine.main() — prints version
        # and creates / destroys an engine handle. Exit 0 == JNI
        # symbols resolved + handle round-trip succeeded.
        add_test(NAME jni_load_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.MediaEngine)
        set_tests_properties(jni_load_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # ---- jni_version_assert: setup fixture --------------------
        # ME_GIT_SHA is computed at cmake configure time and embedded
        # into version.inl. version.cpp gets recompiled when version
        # .inl changes, but libmedia_engine.a is static + the JNI
        # .dylib was linked previously against the old version.cpp.o.
        # Incremental builds that touch a *non-JNI* target (e.g.
        # `cmake --build build --target test_bench_harness`) rebuild
        # media_engine but NOT media_engine_jni — the .dylib still
        # holds the prior cycle's git_sha, and jni_version_assert
        # below tripped a false-positive failure on cycle 77.
        #
        # Fix: ctest fixture that runs `cmake --build` against the
        # JNI lib target before the assert. Adds ~0.5 s when the
        # lib is already current (no-op rebuild check) and ~3-5 s
        # when version.cpp.o needs to be re-linked into the .dylib.
        # Either way the assert sees the lib that matches the
        # configure-time SHA.
        add_test(NAME jni_version_assert_setup
                 COMMAND ${CMAKE_COMMAND}
                         --build ${CMAKE_BINARY_DIR}
                         --target media_engine_jni)
        set_tests_properties(jni_version_assert_setup PROPERTIES
            FIXTURES_SETUP jni_lib_fresh
            TIMEOUT 60)

        # jni_version_assert_smoke: pin nativeVersion()'s return to the
        # build-time C version macros (PROJECT_VERSION_MAJOR/MINOR/PATCH
        # + ME_GIT_SHA — same values that feed src/core/version.inl.in).
        # Catches drift where a future engine-version bump or git_sha
        # field rename leaves the JNI side returning stale data.
        add_test(NAME jni_version_assert_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.VersionAssert
                         "${PROJECT_VERSION_MAJOR}"
                         "${PROJECT_VERSION_MINOR}"
                         "${PROJECT_VERSION_PATCH}"
                         "${ME_GIT_SHA}")
        set_tests_properties(jni_version_assert_smoke PROPERTIES
            DEPENDS           media_engine_jni_classes
            FIXTURES_REQUIRED jni_lib_fresh
            TIMEOUT           20)

        # jni_progress_throw_smoke: install a listener that throws on
        # the 2nd FRAMES event; verify the engine survives + delivers
        # subsequent events. me_jni.cpp:74's ExceptionClear path was
        # uncovered before this — a regression that dropped the
        # ExceptionCheck would crash hosts the moment a listener bug
        # threw.
        add_test(NAME jni_progress_throw_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.ProgressThrow
                         "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4"
                         "${CMAKE_CURRENT_BINARY_DIR}/jni_progress_throw_smoke_out.mp4")
        set_tests_properties(jni_progress_throw_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  30)

        # jni_last_status_smoke: load malformed JSON, expect
        # MediaEngineException with status == ME_E_PARSE (-5). Pins
        # the new structured-status mapping (cycle 84) so future
        # regressions that drop g_jni_last_status assignment can't
        # silently coalesce all failures into status == 0.
        add_test(NAME jni_last_status_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.LastStatusAssert)
        set_tests_properties(jni_last_status_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # Companion check: jni_thumbnail_smoke writes a PNG file but
        # only validates exit code. A regression that returned ME_OK
        # but produced empty / non-PNG bytes would slip through.
        # This test asserts the first 8 bytes match the PNG signature
        # (89 50 4E 47 0D 0A 1A 0A — RFC 2083 §3.1). Sequential after
        # the runner via DEPENDS so the file is on disk when checked.
        add_test(NAME jni_thumbnail_smoke_validate
                 COMMAND bash -c
                   "head -c 8 '${CMAKE_CURRENT_BINARY_DIR}/jni_thumbnail_smoke_out.png' \
                    | od -An -tx1 \
                    | tr -s ' ' \
                    | grep -q '89 50 4e 47 0d 0a 1a 0a'")
        set_tests_properties(jni_thumbnail_smoke_validate PROPERTIES
            DEPENDS jni_thumbnail_smoke
            TIMEOUT 5)

        # jni_cancel_smoke: start a passthrough render, briefly sleep,
        # then cancel + wait. Catches deadlocks on the cancel path
        # (e.g. cancel raced with worker exit) + verifies waitFor
        # returns either ME_OK or ME_E_CANCELLED. TIMEOUT 30s catches
        # a hang.
        add_test(NAME jni_cancel_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.Cancel
                         "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4"
                         "${CMAKE_CURRENT_BINARY_DIR}/jni_cancel_smoke_out.mp4")
        set_tests_properties(jni_cancel_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  30)

        # Companion check: jni_passthrough_smoke writes an MP4 file
        # but only validates exit code. A regression where the
        # passthrough sink returned ME_OK but produced bytes that
        # aren't a valid MP4 container would slip through. Asserts
        # the ISO BMFF `ftyp` box marker at bytes 4-7 (the box-size
        # uint32 occupies 0-3; box-type ASCII follows). Sequential
        # after the runner via DEPENDS so the file is on disk when
        # checked.
        add_test(NAME jni_passthrough_smoke_validate
                 COMMAND bash -c
                   "test \"$(dd if='${CMAKE_CURRENT_BINARY_DIR}/jni_passthrough_smoke_out.mp4' \
                                 bs=1 skip=4 count=4 status=none)\" = ftyp")
        set_tests_properties(jni_passthrough_smoke_validate PROPERTIES
            DEPENDS jni_passthrough_smoke
            TIMEOUT 5)

        # jni_thumbnail_smoke: pull a 160x120 PNG thumbnail at t=0.5s
        # from the determinism fixture via the JNI bridge. Catches
        # nativeThumbnail signature drift + the byte[] copy contract +
        # me_buffer_free ordering. Output PNG lands under the binary
        # dir; ctest re-runs overwrite it.
        add_test(NAME jni_thumbnail_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.Thumbnail
                         "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4"
                         "${CMAKE_CURRENT_BINARY_DIR}/jni_thumbnail_smoke_out.png")
        set_tests_properties(jni_thumbnail_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # jni_passthrough_smoke: end-to-end render via the JNI bridge.
        # io.mediaengine.example.Run builds a passthrough timeline JSON
        # in-memory, calls renderStart → waitFor → close, and exits 0
        # on success. Catches any signature drift in nativeRenderStart
        # / nativeRenderWait + the progress callback trampoline that
        # jni_load_smoke can't reach. Depends on the determinism
        # fixture for a real source MP4. Add the dependency only when
        # the fixture target is in scope (tests/CMakeLists evaluates
        # AFTER bindings/jni/, so determinism_fixture won't exist yet).
        # The check defers to the post-tests scope below.
        set(_me_jni_passthrough_out
            "${CMAKE_CURRENT_BINARY_DIR}/jni_passthrough_smoke_out.mp4")
        add_test(NAME jni_passthrough_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.Run
                         "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4"
                         "${_me_jni_passthrough_out}")
        set_tests_properties(jni_passthrough_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  30)

        # ctest doesn't itself trigger the custom target build; wire
        # the classes target into the default build graph so a normal
        # `cmake --build build` produces the .class file before
        # ctest fires.
        if(NOT TARGET me_default_build_helpers)
            add_custom_target(me_default_build_helpers ALL)
        endif()
        add_dependencies(me_default_build_helpers media_engine_jni_classes)
    else()
        message(STATUS "media_engine_jni: Java toolchain not found — "
                       "skipping jni_load_smoke + jni_passthrough_smoke ctest")
    endif()
endif()
