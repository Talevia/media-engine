include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
)
set(DOCTEST_WITH_TESTS      OFF CACHE INTERNAL "")
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE INTERNAL "")
set(DOCTEST_NO_INSTALL      ON  CACHE INTERNAL "")
# doctest v2.4.11's top-level CMakeLists.txt declares cmake_minimum_required
# below the floor that CMake 4.x enforces. Raise the policy minimum just for
# its configure pass.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)

# One binary per suite so a failing test doesn't abort unrelated suites, and
# so ctest surface matches the file layout at a glance.
set(_test_suites
  test_status
  test_engine
  test_timeline_schema
  test_content_hash
  test_determinism
  test_cache
)

foreach(_suite ${_test_suites})
  add_executable(${_suite} test_main.cpp ${_suite}.cpp)
  target_link_libraries(${_suite} PRIVATE
    media_engine::media_engine
    doctest::doctest)
  add_test(NAME ${_suite} COMMAND ${_suite})
endforeach()

# test_content_hash pokes at internal resource headers (AssetHashCache,
# content_hash) — expose src/ to this one suite the same way
# 02_graph_smoke does for internal validation.
target_include_directories(test_content_hash PRIVATE ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
# Fixture for test_determinism — a tiny deterministic MP4 generated via the
# system ffmpeg CLI at build time. MPEG-4 Part 2 video picked because it's
# built into every LGPL libavcodec (no libx264 needed, so CI won't break if
# the system ffmpeg happens to be LGPL-only). Fixture is rebuilt only if the
# output file is missing or older than this CMakeLists.txt.
# -----------------------------------------------------------------------------
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
  set(_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set(_fixture_mp4 "${_fixture_dir}/determinism_input.mp4")
  add_custom_command(
    OUTPUT ${_fixture_mp4}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
    COMMAND ${FFMPEG_EXECUTABLE} -y -hide_banner -loglevel error
            -f lavfi -i "testsrc=duration=1:size=320x240:rate=10"
            -c:v mpeg4 -q:v 5
            ${_fixture_mp4}
    DEPENDS ${CMAKE_CURRENT_LIST_FILE}
    COMMENT "Generating determinism test fixture: ${_fixture_mp4}"
    VERBATIM)
  add_custom_target(determinism_fixture DEPENDS ${_fixture_mp4})
  add_dependencies(test_determinism determinism_fixture)
  target_compile_definitions(test_determinism PRIVATE
    ME_TEST_FIXTURE_MP4="${_fixture_mp4}")
else()
  message(STATUS "ffmpeg CLI not found — test_determinism will skip at runtime")
endif()
