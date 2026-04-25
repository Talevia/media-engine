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
        set(_me_jni_classdir "${CMAKE_CURRENT_BINARY_DIR}/classes")
        set(_me_jni_classfile
            "${_me_jni_classdir}/io/mediaengine/MediaEngine.class")
        set(_me_jni_run_classfile
            "${_me_jni_classdir}/io/mediaengine/example/Run.class")

        # Compile MediaEngine.java + Run.java together (Run depends on
        # the wrapper class). Re-runs only when either .java changes.
        # Keep the classes/ tree under the binary dir so a clean build
        # wipes it.
        add_custom_command(
            OUTPUT  "${_me_jni_classfile}" "${_me_jni_run_classfile}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_me_jni_classdir}"
            COMMAND ${Java_JAVAC_EXECUTABLE}
                    -d "${_me_jni_classdir}"
                    "${_me_jni_java_src}"
                    "${_me_jni_run_src}"
            DEPENDS "${_me_jni_java_src}" "${_me_jni_run_src}"
            COMMENT "javac MediaEngine.java + Run.java -> ${_me_jni_classdir}")

        add_custom_target(media_engine_jni_classes
            DEPENDS "${_me_jni_classfile}" "${_me_jni_run_classfile}")
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
