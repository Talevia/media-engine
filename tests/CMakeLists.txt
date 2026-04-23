include(FetchContent)

# FFmpeg imported targets are created at src/ scope by the sibling CMakeLists;
# re-find here so tests/ sees FFMPEG::avcodec et al. for the fixture generator.
# FindFFMPEG guards target creation with `if(NOT TARGET ...)`, so this is safe
# to call in multiple scopes.
find_package(FFMPEG REQUIRED)

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
# below CMake 4.x's floor. Shared helper sets CMAKE_POLICY_VERSION_MINIMUM
# so every FetchContent caller that needs this workaround lands it the
# same way. See `cmake/fetchcontent_policy.cmake` for details.
include(fetchcontent_policy)
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
  test_output_sink
  test_probe
  test_thumbnail
  test_asset_reuse
  test_timeline_segment
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

# test_timeline_schema verifies loader produces the right IR — reaches
# into me::Timeline / me::Asset to check parsed Asset::color_space values.
target_include_directories(test_timeline_schema PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_timeline_segment calls me::timeline::segment() on hand-built
# Timeline IR — same src/-as-include pattern as test_timeline_schema.
target_include_directories(test_timeline_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_output_sink calls the internal factory me::orchestrator::make_output_sink
# directly — same src/-as-include pattern as the other internal-surface suites.
target_include_directories(test_output_sink PRIVATE ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
# Fixture for test_determinism — a tiny deterministic MP4 generated at build
# time by `gen_fixture`, a small helper that links libavcodec / libavformat
# directly. MPEG-4 Part 2 video picked because it ships with every LGPL
# libavcodec (no libx264 needed). Using libav at fixture-gen time removes the
# previous dependency on a system ffmpeg CLI, so CI without ffmpeg-on-PATH no
# longer silently skips the determinism regression. Fixture is rebuilt only if
# gen_fixture itself or this CMakeLists.txt changes.
# -----------------------------------------------------------------------------
add_executable(gen_fixture fixtures/gen_fixture.cpp)
target_link_libraries(gen_fixture PRIVATE
  FFMPEG::avformat
  FFMPEG::avcodec
  FFMPEG::avutil)

set(_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(_fixture_mp4 "${_fixture_dir}/determinism_input.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_fixture> ${_fixture_mp4}
  DEPENDS gen_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating determinism test fixture: ${_fixture_mp4}"
  VERBATIM)
add_custom_target(determinism_fixture DEPENDS ${_fixture_mp4})
add_dependencies(test_determinism determinism_fixture)
target_compile_definitions(test_determinism PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_probe reuses the determinism fixture — same build-time MP4 gives
# us a known codec / dimensions / framerate / color-tag state to assert
# probe output against.
add_dependencies(test_probe determinism_fixture)
target_compile_definitions(test_probe PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_thumbnail also reuses the determinism fixture — the PNG encoder
# asserts pin down dimensions (640×480 native, scale-to-bound variants),
# PNG magic + IHDR. Same rationale as test_probe for fixture reuse.
add_dependencies(test_thumbnail determinism_fixture)
target_compile_definitions(test_thumbnail PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")
