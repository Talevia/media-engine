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

add_library(media_engine_jni SHARED me_jni.cpp me_jni_progress.cpp)

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
        set(_me_jni_classdir "${CMAKE_CURRENT_BINARY_DIR}/classes")
        # Shared fixture path — every JNI ctest that needs a real
        # source MP4 reads this. Mirrors the construction rule in
        # tests/CMakeLists.txt without depending on it (tests/ is a
        # separate add_subdirectory; we don't reach into its scope).
        # If the fixture relocates, this is the single edit point
        # for the JNI side.
        set(_me_jni_test_fixture_mp4
            "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4")

        # All Java sources compiled together as one javac invocation.
        # Each entry is the path under bindings/jni/ relative to the
        # CMakeLists; the loop below derives the absolute .java path
        # AND the matching .class output. Add new demos to this list
        # only — no parallel _src/_classfile var pairs anymore.
        #
        # Path convention: every entry sits under either
        #   src/io/mediaengine/<Class>.java          (wrapper)
        # or
        #   example/src/io/mediaengine/example/<Class>.java
        # The .class output mirrors the package path:
        #   classes/io/mediaengine/<Class>.class
        # / classes/io/mediaengine/example/<Class>.class
        set(_me_jni_java_sources
            src/io/mediaengine/MediaEngine.java
            src/io/mediaengine/Frames.java
            example/src/io/mediaengine/example/Run.java
            example/src/io/mediaengine/example/VersionAssert.java
            example/src/io/mediaengine/example/Thumbnail.java
            example/src/io/mediaengine/example/ThumbnailBatch.java
            example/src/io/mediaengine/example/ConfigCacheAssert.java
            example/src/io/mediaengine/example/ThumbnailArgsAssert.java
            example/src/io/mediaengine/example/Cancel.java
            example/src/io/mediaengine/example/LastStatusAssert.java
            example/src/io/mediaengine/example/ProgressThrow.java
            example/src/io/mediaengine/example/FrameFetch.java
            example/src/io/mediaengine/example/Timelines.java
        )

        set(_me_jni_java_abs_sources)
        set(_me_jni_class_outputs)
        foreach(_rel_src IN LISTS _me_jni_java_sources)
            list(APPEND _me_jni_java_abs_sources
                "${CMAKE_CURRENT_SOURCE_DIR}/${_rel_src}")
            # Strip the source-tree prefix to get the package-relative
            # path; both `src/` and `example/src/` map to the same
            # output root. e.g.
            #   src/io/mediaengine/MediaEngine.java
            #     → io/mediaengine/MediaEngine.class
            #   example/src/io/mediaengine/example/Run.java
            #     → io/mediaengine/example/Run.class
            string(REGEX REPLACE "^example/src/" "" _pkg_path "${_rel_src}")
            string(REGEX REPLACE "^src/"         "" _pkg_path "${_pkg_path}")
            string(REGEX REPLACE "\\.java$"      ".class" _class_rel "${_pkg_path}")
            list(APPEND _me_jni_class_outputs
                "${_me_jni_classdir}/${_class_rel}")
        endforeach()

        # Compile MediaEngine.java + the example runners together; the
        # runners depend on the wrapper class. Re-runs only when any
        # .java in the input list changes. Keep the classes/ tree
        # under the binary dir so a clean build wipes it.
        add_custom_command(
            OUTPUT  ${_me_jni_class_outputs}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_me_jni_classdir}"
            COMMAND ${Java_JAVAC_EXECUTABLE}
                    -d "${_me_jni_classdir}"
                    ${_me_jni_java_abs_sources}
            DEPENDS ${_me_jni_java_abs_sources}
            COMMENT "javac MediaEngine.java + example runners -> ${_me_jni_classdir}")

        add_custom_target(media_engine_jni_classes
            DEPENDS ${_me_jni_class_outputs})
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
                         "${_me_jni_test_fixture_mp4}"
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

        # jni_thumbnail_args_smoke: pin the cycle 107 contract that
        # MediaEngine.thumbnail rejects negative maxWidth/Height
        # with IllegalArgumentException. No fixture needed —
        # bad-arg check happens in Java before the JNI bridge fires.
        add_test(NAME jni_thumbnail_args_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.ThumbnailArgsAssert)
        set_tests_properties(jni_thumbnail_args_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  10)

        # jni_config_cache_smoke: pin the cycle 106 MediaEngine
        # (Config) ctor — passing a non-null cacheDir engages the
        # engine's disk cache (verifiable via files appearing under
        # the cacheDir). Catches a regression that drops the
        # cache_dir field on the JNI side; without it host hosts
        # silently run without disk cache.
        set(_me_jni_config_cache_dir
            "${CMAKE_CURRENT_BINARY_DIR}/jni_config_cache_smoke_dir")
        add_test(NAME jni_config_cache_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.ConfigCacheAssert
                         "${_me_jni_test_fixture_mp4}"
                         "${_me_jni_config_cache_dir}")
        set_tests_properties(jni_config_cache_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # jni_thumbnail_batch_smoke: scrub-row pattern — fetch 5
        # PNGs across the source's first 2 seconds, write each.
        # Verifies engine handle reuse + per-thumbnail status across
        # repeated calls (the cycle 76 jni_thumbnail_smoke only
        # tested one fetch).
        set(_me_jni_thumb_batch_out
            "${CMAKE_CURRENT_BINARY_DIR}/jni_thumbnail_batch_smoke_out")
        add_test(NAME jni_thumbnail_batch_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.ThumbnailBatch
                         "${_me_jni_test_fixture_mp4}"
                         "${_me_jni_thumb_batch_out}")
        set_tests_properties(jni_thumbnail_batch_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # Companion check: verify all 5 PNG files were written with
        # PNG signature. Same magic-byte pattern as cycle 87's
        # jni_thumbnail_smoke_validate.
        add_test(NAME jni_thumbnail_batch_smoke_validate
                 COMMAND bash -c
                   "for i in 0 1 2 3 4; do \
                      head -c 8 '${_me_jni_thumb_batch_out}/thumb_'$i'.png' \
                        | od -An -tx1 \
                        | tr -s ' ' \
                        | grep -q '89 50 4e 47 0d 0a 1a 0a' \
                        || { echo missing-or-bad-png: thumb_$i; exit 1; }; \
                    done")
        set_tests_properties(jni_thumbnail_batch_smoke_validate PROPERTIES
            DEPENDS jni_thumbnail_batch_smoke
            TIMEOUT 5)

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
                         "${_me_jni_test_fixture_mp4}"
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

        # jni_frame_smoke: fetch a single RGBA frame at t=0.5s through
        # the me_render_frame JNI bridge + write a PPM. Catches
        # nativeRenderFrame signature drift + Frame record contract +
        # the byte[] copy semantics.
        add_test(NAME jni_frame_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.FrameFetch
                         "${_me_jni_test_fixture_mp4}"
                         "${CMAKE_CURRENT_BINARY_DIR}/jni_frame_smoke_out.ppm")
        set_tests_properties(jni_frame_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # Companion check: verify the FrameFetch output is a valid
        # P6 PPM (bytes 0-2 == "P6\n"). A regression where
        # nativeRenderFrame returned ME_OK but the byte[] copy
        # produced empty / corrupt RGBA would surface as a
        # mis-headered or zero-byte file. Mirrors cycle 87/88
        # validators.
        add_test(NAME jni_frame_smoke_validate
                 COMMAND bash -c
                   "test \"$(head -c 2 '${CMAKE_CURRENT_BINARY_DIR}/jni_frame_smoke_out.ppm')\" = P6")
        set_tests_properties(jni_frame_smoke_validate PROPERTIES
            DEPENDS jni_frame_smoke
            TIMEOUT 5)

        # jni_frame_png_smoke: same FrameFetch with a `.png` output —
        # exercises `io.mediaengine.Frames.toBufferedImage` plus
        # `javax.imageio.ImageIO.write`. Catches regressions in the
        # RGBA8 → BufferedImage TYPE_INT_ARGB pack (channel order, row
        # length consistency) that the PPM path would silently swallow.
        add_test(NAME jni_frame_png_smoke
                 COMMAND ${Java_JAVA_EXECUTABLE}
                         "-Djava.library.path=$<TARGET_FILE_DIR:media_engine_jni>"
                         -cp "${_me_jni_classdir}"
                         io.mediaengine.example.FrameFetch
                         "${_me_jni_test_fixture_mp4}"
                         "${CMAKE_CURRENT_BINARY_DIR}/jni_frame_png_smoke_out.png")
        set_tests_properties(jni_frame_png_smoke PROPERTIES
            DEPENDS  media_engine_jni_classes
            TIMEOUT  20)

        # Companion check: PNG magic = 89 50 4E 47 0D 0A 1A 0A. We
        # match the first 4 bytes via bash hex comparison — a
        # regression where Frames.toBufferedImage produced a
        # malformed image (wrong dimensions, all-zero pixels — both
        # write_png-survivable but spec-violating) would still pass
        # this smoke check, but a write that didn't reach disk or
        # truncated mid-IHDR fails. Stronger pixel-level checks would
        # need a Java-side asserter; tracked as future work if PNG
        # path picks up more contracts.
        add_test(NAME jni_frame_png_smoke_validate
                 COMMAND bash -c
                   "test \"$(xxd -p -l 4 '${CMAKE_CURRENT_BINARY_DIR}/jni_frame_png_smoke_out.png')\" = 89504e47")
        set_tests_properties(jni_frame_png_smoke_validate PROPERTIES
            DEPENDS jni_frame_png_smoke
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
                         "${_me_jni_test_fixture_mp4}"
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
                         "${_me_jni_test_fixture_mp4}"
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
