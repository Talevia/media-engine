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
  test_color_pipeline
  test_render_cancel
  test_render_progress
  test_render_job_lifecycle
  test_output_spec
  test_compose_alpha_over
  test_compose_active_clips
  test_compose_frame_convert
  test_audio_mix
  test_compose_cross_dissolve
  test_audio_resample
  test_frame_puller
  test_compose_sink_e2e
  test_compose_affine_blit
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

# test_compose_alpha_over drives me::compose::alpha_over() from
# `src/compose/alpha_over.hpp` with known RGBA8 triples to pin the
# per-blend-mode math. Same src/-as-include pattern as the other
# internal-surface suites.
target_include_directories(test_compose_alpha_over PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_active_clips hand-builds me::Timeline IR to drive
# me::compose::active_clips_at — needs `src/` on the include path
# for `timeline/timeline_impl.hpp` (same pattern as
# test_timeline_segment / test_timeline_schema).
target_include_directories(test_compose_active_clips PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_frame_convert needs libav headers (AVFrame construction
# for synthetic YUV test data). FFMPEG imports already available via
# media_engine PRIVATE deps, but the test sees only the public
# media_engine symbols; re-link avutil + avcodec here explicitly.
target_include_directories(test_compose_frame_convert PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_compose_frame_convert PRIVATE
  FFMPEG::avutil FFMPEG::avcodec FFMPEG::swscale)

# test_audio_mix exercises me::audio::mix_samples / peak_limiter /
# db_to_linear from src/audio/mix.hpp — pure math, needs src/ on
# the include path.
target_include_directories(test_audio_mix PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_cross_dissolve pins cross_dissolve lerp math via
# known RGBA values at canonical t points. Same src/-as-include
# pattern as the other me::compose tests.
target_include_directories(test_compose_cross_dissolve PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_affine_blit pins the affine matrix + blit kernel math.
target_include_directories(test_compose_affine_blit PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_audio_resample constructs synthetic AVFrames to drive
# me::audio::resample_to — needs libav headers + src/ include.
target_include_directories(test_audio_resample PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_resample PRIVATE
  FFMPEG::avutil FFMPEG::avcodec FFMPEG::swresample)

# test_frame_puller opens the determinism fixture directly via libav
# (avformat_open_input / avcodec decoder) to exercise the
# pull_next_video_frame state machine. Needs src/ include + libav
# link. The fixture-path define + dependency are set below, after
# _fixture_mp4 is defined (mirrors the test_determinism / test_probe
# pattern — CMake evaluates variables top-to-bottom).
target_include_directories(test_frame_puller PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_frame_puller PRIVATE
  FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)

# test_color_pipeline instantiates me::color::make_pipeline() from the
# internal `src/color/pipeline.hpp`; uses `me::ColorSpace` from
# `src/timeline/timeline_impl.hpp` to exercise the abstract Pipeline
# contract with typed color-space tags. Mirrors the ME_HAS_OCIO compile
# definition from media_engine so the OcioPipeline-specific test cases
# (behind `#if ME_HAS_OCIO`) participate in the build when OCIO is on.
target_include_directories(test_color_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_OCIO)
  target_compile_definitions(test_color_pipeline PRIVATE ME_HAS_OCIO=1)
endif()

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

# Tagged variant: same shape as the main fixture but with bt709 /
# limited color metadata set on the encoder (see gen_fixture's
# --tagged branch). Scaffold for color-aware thumbnail / probe tests;
# today the tags don't all survive into the MP4 container (MPEG-4
# Part 2 in mov drops most), so tests that use it stay on structural
# invariants (valid PNG, valid probe) rather than strict tag round-trip.
set(_fixture_mp4_tagged "${_fixture_dir}/determinism_input_tagged.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4_tagged}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_fixture> --tagged ${_fixture_mp4_tagged}
  DEPENDS gen_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating tagged determinism test fixture: ${_fixture_mp4_tagged}"
  VERBATIM)
add_custom_target(determinism_fixture_tagged DEPENDS ${_fixture_mp4_tagged})

# Audio-capable variant: same video stream + a silent AAC audio stream.
# Consumers: audio-mix pipeline tests that need a real audio stream
# to exercise pull_next_audio_frame / the planned AudioMixScheduler.
set(_fixture_mp4_with_audio "${_fixture_dir}/determinism_input_with_audio.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4_with_audio}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_fixture> --with-audio ${_fixture_mp4_with_audio}
  DEPENDS gen_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating audio-capable determinism test fixture: ${_fixture_mp4_with_audio}"
  VERBATIM)
add_custom_target(determinism_fixture_with_audio DEPENDS ${_fixture_mp4_with_audio})

# test_probe reuses the determinism fixture — same build-time MP4 gives
# us a known codec / dimensions / framerate / color-tag state to assert
# probe output against.
add_dependencies(test_probe determinism_fixture)
target_compile_definitions(test_probe PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_thumbnail also reuses the determinism fixture — the PNG encoder
# asserts pin down dimensions (640×480 native, scale-to-bound variants),
# PNG magic + IHDR. Same rationale as test_probe for fixture reuse.
# Also depends on the tagged variant so one case can assert thumbnail
# still produces a valid PNG from a color-metadata-carrying source (a
# canary for future color-aware thumbnail work when M2 compose
# lands OCIO wiring).
add_dependencies(test_thumbnail determinism_fixture determinism_fixture_tagged)
target_compile_definitions(test_thumbnail PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
  ME_TEST_FIXTURE_MP4_TAGGED="${_fixture_mp4_tagged}")

# test_render_cancel exercises me_render_start → me_render_cancel →
# me_render_wait against the shared determinism fixture. Uses a 3-clip
# h264/aac reencode timeline (not passthrough) + 200ms sleep so the
# worker is reliably mid-encode when cancel fires — passthrough
# stream-copy finishes in under 200ms even at 10 clips on dev hardware.
add_dependencies(test_render_cancel determinism_fixture)
target_compile_definitions(test_render_cancel PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_frame_puller wires its fixture path + dependency now that
# _fixture_mp4 is defined. Also picks up the audio-capable variant
# for the pull_next_audio_frame drain test (video-only fixture
# makes that path fall through to the skip branch).
add_dependencies(test_frame_puller determinism_fixture determinism_fixture_with_audio)
target_compile_definitions(test_frame_puller PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_compose_sink_e2e builds a 2-track timeline using the shared
# fixture twice (both tracks reference the same asset; loader creates
# independent DemuxContexts per clip so the compose loop sees two
# real decoded streams).
add_dependencies(test_compose_sink_e2e determinism_fixture)
target_compile_definitions(test_compose_sink_e2e PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_render_progress exercises the me_progress_event_t ordering contract
# (STARTED first / terminal last / ratio monotonic) — uses the shared
# fixture for a single-clip passthrough on the happy path and a 3-clip
# reencode for the cancel path (same reasoning as test_render_cancel).
add_dependencies(test_render_progress determinism_fixture)
target_compile_definitions(test_render_progress PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_render_job_lifecycle pins me_render_job_destroy corners —
# destroy(NULL) idempotency, destroy-without-wait, start → cancel →
# destroy-without-wait, standard cycle followed by NULL destroy. Shares
# the same fixture as the other render-path suites.
add_dependencies(test_render_job_lifecycle determinism_fixture)
target_compile_definitions(test_render_job_lifecycle PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_output_spec covers me_output_spec_t.container inference corners:
# null-container happy paths (mp4 / mov extension inference) and the
# two failure modes (unknown extension, explicit unknown container name).
add_dependencies(test_output_spec determinism_fixture)
target_compile_definitions(test_output_spec PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")
