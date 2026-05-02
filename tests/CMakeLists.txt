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
  test_timeline_schema_audio
  test_timeline_schema_clip_attributes
  test_timeline_schema_color_space
  test_timeline_schema_ml_asset
  test_engine_ml
  test_timeline_schema_effects
  test_timeline_schema_transitions
  test_content_hash
  test_determinism
  test_cache
  test_output_sink
  test_probe
  test_thumbnail
  test_compose_png
  test_frame_pool
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
  test_compose_sink_e2e_audio
  test_compose_sink_e2e_text
  test_compose_sink_e2e_subtitle
  test_frame_server_concurrent
  test_compose_affine_blit
  test_render_affine_blit
  test_render_compose_cpu
  test_render_cross_dissolve
  test_compose_multi_track_graph
  test_render_encode_png
  test_stateful_pool
  test_audio_track_feed
  test_audio_mixer
  test_effect_chain
  test_animated_number
  test_animated_color
  test_gpu_backend
  test_animated_number_loader
  test_compose_sink_gpu_detect
  test_render_thread
  test_lut_parser
  test_tempo
  test_audio_effect_chain
  test_reencode_pts_remap
  test_encode_hevc_main10
  test_tonemap_kernel
  test_inverse_tonemap_stub
  test_face_sticker_stub
  test_face_mosaic_stub
  test_body_alpha_key_stub
  test_hdr_metadata_propagate
  test_pq_hlg_roundtrip
  test_decode_vp9_profile2
  test_decode_av1_10bit
  test_subtitle_renderer
  test_skia_backend
  test_text_renderer
  test_font_fallback
  test_frame_server
  test_disk_cache
  test_bench_harness
  test_decode_kernels
  test_io_decode_audio
  test_audio_resample_kernel
  test_audio_mix_kernel
  test_audio_timestretch_kernel
  test_audio_chunk_graph
  test_scheduler_cache
  test_player
  test_api_surface_budget
  test_cache_hit_rate_lower_bound
  test_inference_coreml_skeleton
  test_inference_multiarray_layout
  test_inference_onnx_skeleton
  test_kvazaar_hevc_encoder
  test_hevc_sw_sink
  test_inference_content_hash_cache
  test_inference_license_whitelist
  test_effect_param_ml_asset_ref
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

# test_timeline_schema* — core + audio + clip_attributes + transitions
# shards of the original 1535-line suite (split per
# debt-split-test-timeline-schema-cpp). All four files share
# fixtures via tests/timeline_schema_fixtures.hpp and reach into
# `src/` to check me::Timeline / me::Asset IR directly.
target_include_directories(test_timeline_schema
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_audio
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_clip_attributes
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_color_space
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_ml_asset
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_effects
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_timeline_schema_transitions
                            PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_timeline_segment calls me::timeline::segment() on hand-built
# Timeline IR — same src/-as-include pattern as test_timeline_schema.
target_include_directories(test_timeline_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_bench_harness includes bench/bench_harness.hpp directly to test
# the template's behaviour in isolation (the two bench targets only
# exercise the happy path transitively).
target_include_directories(test_bench_harness PRIVATE ${CMAKE_SOURCE_DIR}/bench)

# test_api_surface_budget — VISION §5.7-4 guard. Receives header
# scan path + budget file path as compile definitions so it works
# regardless of ctest invocation cwd.
target_compile_definitions(test_api_surface_budget PRIVATE
  ME_TEST_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/include"
  ME_TEST_API_BUDGET_FILE="${CMAKE_SOURCE_DIR}/tools/api_surface_budget.txt")

# test_cache_hit_rate_lower_bound — VISION §5.7-6 guard. Loads the
# determinism fixture timeline JSON; fixture paths injected the same
# way as bench_thumbnail_png so the test is invocation-cwd-independent.
add_dependencies(test_cache_hit_rate_lower_bound determinism_fixture)
target_compile_definitions(test_cache_hit_rate_lower_bound PRIVATE
  ME_TEST_FIXTURE_MP4="${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4")

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

# test_render_affine_blit exercises the RenderAffineBlit kernel ABI
# (graph::Properties → InputValue → OutputSlot). Reaches into src/
# for task/registry + graph/types — the C API doesn't expose graph
# evaluation, by design.
target_include_directories(test_render_affine_blit PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_render_compose_cpu exercises the RenderComposeCpu kernel + the
# Builder's variadic_last_input path.
target_include_directories(test_render_compose_cpu PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_render_cross_dissolve exercises the RenderCrossDissolve kernel ABI.
target_include_directories(test_render_cross_dissolve PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_multi_track_graph asserts compile_compose_graph topology
# for hand-built Timeline IR (single/multi track, transform, gaps).
target_include_directories(test_compose_multi_track_graph PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_render_encode_png exercises the RenderEncodePng kernel ABI.
target_include_directories(test_render_encode_png PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_stateful_pool exercises the StatefulResourcePool template.
target_include_directories(test_stateful_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_audio_resample constructs synthetic AVFrames to drive
# me::audio::resample_to — needs libav headers + src/ include.
target_include_directories(test_audio_resample PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_resample PRIVATE
  FFMPEG::avutil FFMPEG::avcodec FFMPEG::swresample)

# test_audio_track_feed opens the --with-audio fixture and drains it
# through me::audio::AudioTrackFeed (decode → resample → gain). Needs
# src/ include + libav (avformat/avcodec/avutil for the demux+decode
# half; resample_to links swresample internally).
target_include_directories(test_audio_track_feed PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_track_feed PRIVATE
  FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)

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
# test_determinism's fixture deps + defines wire below after the
# with-audio variant is declared (CMake variable ordering — same
# pattern as test_frame_puller further down).

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

# HDR fixture variant: HEVC Main 10 + P010 + AVMasteringDisplayMetadata
# (BT.2020 primaries, 0.0001–1000 nits) + AVContentLightMetadata
# (MaxCLL=1000, MaxFALL=400). Built by gen_hdr_fixture which uses
# hevc_videotoolbox; gracefully degrades to "no fixture, exit 0" on
# hosts without VideoToolbox (Linux / Windows). Tests that consume
# this fixture (test_probe positive HDR case + the future
# test-hdr-metadata-propagate / test-pq-hlg-roundtrip / bench-
# hdr-roundtrip cycles) check the fixture's existence + size at
# runtime via ME_REQUIRE_FIXTURE before asserting on it.
add_executable(gen_hdr_fixture fixtures/gen_hdr_fixture.cpp)
target_link_libraries(gen_hdr_fixture PRIVATE
  FFMPEG::avformat
  FFMPEG::avcodec
  FFMPEG::avutil)

set(_fixture_mp4_hdr "${_fixture_dir}/hdr_input.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4_hdr}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_hdr_fixture> ${_fixture_mp4_hdr}
  DEPENDS gen_hdr_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating HDR test fixture: ${_fixture_mp4_hdr}"
  VERBATIM)
add_custom_target(hdr_fixture DEPENDS ${_fixture_mp4_hdr})

# test_determinism depends on both fixture variants now — the video-
# only fixture for legacy passthrough / h264 reencode cases, and the
# with-audio fixture for the new compose-path determinism case.
add_dependencies(test_determinism determinism_fixture determinism_fixture_with_audio)
target_compile_definitions(test_determinism PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_probe also consumes the HDR fixture for the
# `me_media_info_video_hdr_metadata` positive-path TEST_CASE
# (`debt-probe-hdr-positive-fixture`). Same ME_REQUIRE_FIXTURE skip
# pattern as the SDR fixture deps above.
add_dependencies(test_probe hdr_fixture)
target_compile_definitions(test_probe PRIVATE
  ME_TEST_FIXTURE_MP4_HDR="${_fixture_mp4_hdr}")

# test_hdr_metadata_propagate exercises the full HDR re-encode
# round-trip (probe HDR → render with video_codec="hevc" → re-probe).
# Same fixture + skip pattern as test_probe's HDR positive case.
add_dependencies(test_hdr_metadata_propagate hdr_fixture)
target_compile_definitions(test_hdr_metadata_propagate PRIVATE
  ME_TEST_FIXTURE_MP4_HDR="${_fixture_mp4_hdr}")

# test_pq_hlg_roundtrip exercises pixel-level HDR round-trip
# (decode → re-encode (HEVC) → decode → ε-compare). Same fixture
# + skip pattern. ε-tolerance covers the HW encoder's
# non-deterministic but bounded drift.
add_dependencies(test_pq_hlg_roundtrip hdr_fixture)
target_compile_definitions(test_pq_hlg_roundtrip PRIVATE
  ME_TEST_FIXTURE_MP4_HDR="${_fixture_mp4_hdr}")

# VP9 Profile 2 fixture: 10-bit YUV 4:2:0 SDR mp4 produced by
# libvpx-vp9 (LGPL-clean BSD-2-clause, ships with Homebrew FFmpeg
# per --enable-libvpx). Covers M10 exit criterion §M10:119 second
# leg — VP9 P2 decode reuses the same 10-bit libswscale path that
# HEVC Main 10's P010 hits, so the test is structural ("decode
# produces sensible RGBA8") rather than a full ε round-trip.
# Fixture generator gracefully degrades to "exit 0, no output" on
# hosts without libvpx-vp9; consumers skip via ME_REQUIRE_FIXTURE.
add_executable(gen_vp9_p2_fixture fixtures/gen_vp9_p2_fixture.cpp)
target_link_libraries(gen_vp9_p2_fixture PRIVATE
  FFMPEG::avformat
  FFMPEG::avcodec
  FFMPEG::avutil)

set(_fixture_mp4_vp9p2 "${_fixture_dir}/vp9_p2_input.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4_vp9p2}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_vp9_p2_fixture> ${_fixture_mp4_vp9p2}
  DEPENDS gen_vp9_p2_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating VP9 Profile 2 test fixture: ${_fixture_mp4_vp9p2}"
  VERBATIM)
add_custom_target(vp9_p2_fixture DEPENDS ${_fixture_mp4_vp9p2})

add_dependencies(test_decode_vp9_profile2 vp9_p2_fixture)
target_compile_definitions(test_decode_vp9_profile2 PRIVATE
  ME_TEST_FIXTURE_MP4_VP9P2="${_fixture_mp4_vp9p2}")

# AV1 Main 10-bit fixture: yuv420p10le SDR mp4 produced by
# libsvtav1 (BSD-3-clause, ships with Homebrew FFmpeg per
# --enable-libsvtav1). Closes M10 exit criterion §M10:119 third
# leg — AV1 10-bit decode reuses the same frame_to_rgba8 path as
# VP9 P2 / HEVC Main 10. Decoder side is libdav1d (BSD-2) auto-
# routed by libavformat. Same skip pattern as the other 10-bit
# fixtures via ME_REQUIRE_FIXTURE.
add_executable(gen_av1_10bit_fixture fixtures/gen_av1_10bit_fixture.cpp)
target_link_libraries(gen_av1_10bit_fixture PRIVATE
  FFMPEG::avformat
  FFMPEG::avcodec
  FFMPEG::avutil)

set(_fixture_mp4_av1_10 "${_fixture_dir}/av1_10bit_input.mp4")
add_custom_command(
  OUTPUT ${_fixture_mp4_av1_10}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_fixture_dir}
  COMMAND $<TARGET_FILE:gen_av1_10bit_fixture> ${_fixture_mp4_av1_10}
  DEPENDS gen_av1_10bit_fixture ${CMAKE_CURRENT_LIST_FILE}
  COMMENT "Generating AV1 10-bit test fixture: ${_fixture_mp4_av1_10}"
  VERBATIM)
add_custom_target(av1_10bit_fixture DEPENDS ${_fixture_mp4_av1_10})

add_dependencies(test_decode_av1_10bit av1_10bit_fixture)
target_compile_definitions(test_decode_av1_10bit PRIVATE
  ME_TEST_FIXTURE_MP4_AV1_10="${_fixture_mp4_av1_10}")

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

# test_frame_pool pins FramePool's budget / pressure / deleter
# contract (debt-frame-pool-enforce-budget). Reaches into src/ for
# the internal class directly.
target_include_directories(test_frame_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_compose_png exercises me::orchestrator::compose_png_at
# (composition-level thumbnail — distinct from me_thumbnail_png's
# asset-level path). Reaches into src/ to instantiate the class
# directly; no C API exposure yet.
target_include_directories(test_compose_png PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_dependencies(test_compose_png determinism_fixture)
target_compile_definitions(test_compose_png PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

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

# test_audio_track_feed drives the feed off the audio-capable fixture
# (primary) + needs the video-only fixture to test the "no audio
# stream" branch. Both deps.
add_dependencies(test_audio_track_feed determinism_fixture determinism_fixture_with_audio)
target_compile_definitions(test_audio_track_feed PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_audio_mixer drives me::audio::AudioMixer with 1-2 AudioTrackFeed
# instances off the audio-capable fixture. Needs src/ include +
# libav link (the test opens demuxes + frees AVFrames).
target_include_directories(test_audio_mixer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_mixer PRIVATE
  FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)

# test_effect_chain exercises me::effect::EffectChain + IdentityEffect
# via the headers in src/effect/. Pure C++ (no libav, no filesystem).
target_include_directories(test_effect_chain PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_animated_number exercises me::AnimatedNumber (M3 keyframe
# interpolation primitive). Pure C++ (no libav).
target_include_directories(test_animated_number PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_animated_color exercises the AnimatedColor primitive + its
# loader (parse_animated_color + parse_text_clip_params color
# field). Reaches into src/ like the sibling animated_number
# test; pulls in nlohmann/json via loader_helpers.hpp.
target_include_directories(test_animated_color PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_animated_color PRIVATE nlohmann_json::nlohmann_json)

# test_gpu_backend exercises both GpuBackend implementations: default
# NullGpuBackend (phase-1) and BgfxGpuBackend (ME_WITH_GPU=ON). Mirror
# the `ME_HAS_GPU` compile def from media_engine so the bgfx-specific
# test cases (behind `#ifdef ME_HAS_GPU`) participate in the build
# when GPU is on.
target_include_directories(test_gpu_backend PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_GPU)
  target_compile_definitions(test_gpu_backend PRIVATE ME_HAS_GPU=1)
endif()

# test_bgfx_metal_render is the pixel-level render-proof test that
# closes M3's "bgfx 集成，macOS Metal 后端可渲染" exit criterion. Only
# built when ME_WITH_GPU=ON — the entire test file is #ifdef
# ME_HAS_GPU-guarded, and without bgfx linked there's no way to call
# bgfx::createFrameBuffer / readTexture. Links bgfx + bx directly
# (not just via media_engine, which has them PRIVATE) so the test
# TU's #include <bgfx/bgfx.h> resolves + the bgfx symbols link.
if(ME_WITH_GPU)
  add_executable(test_bgfx_metal_render test_main.cpp test_bgfx_metal_render.cpp)
  target_link_libraries(test_bgfx_metal_render PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_bgfx_metal_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_bgfx_metal_render PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_bgfx_metal_render COMMAND test_bgfx_metal_render)
endif()

# test_gpu_effect_chain exercises me::effect::GpuEffect +
# GpuEffectChain plumbing (ping-pong scratch dispatch, view-id
# allocation, Noop effect smoke). Same ME_WITH_GPU gate + bgfx link
# pattern as test_bgfx_metal_render.
if(ME_WITH_GPU)
  add_executable(test_gpu_effect_chain test_main.cpp test_gpu_effect_chain.cpp)
  target_link_libraries(test_gpu_effect_chain PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_gpu_effect_chain PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_gpu_effect_chain PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_gpu_effect_chain COMMAND test_gpu_effect_chain)
endif()

# test_bgfx_noop_fallback exercises the `ME_GPU_FORCE_NOOP` env-var
# hook that forces BgfxGpuBackend into the Noop renderer branch —
# the branch CI otherwise can't reach on dev macOS where Metal
# auto-pick always succeeds.
if(ME_WITH_GPU)
  add_executable(test_bgfx_noop_fallback test_main.cpp test_bgfx_noop_fallback.cpp)
  target_link_libraries(test_bgfx_noop_fallback PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_bgfx_noop_fallback PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_bgfx_noop_fallback PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_bgfx_noop_fallback COMMAND test_bgfx_noop_fallback)
endif()

# test_color_correct_effect verifies the first real GpuEffect's
# fragment-shader math (brightness / contrast / saturation) via
# pixel readback. ME_WITH_GPU-only; skips on non-Metal at runtime
# (shader bytecode today is Metal-only — see
# src/effect/shaders/README.md).
if(ME_WITH_GPU)
  add_executable(test_color_correct_effect test_main.cpp test_color_correct_effect.cpp)
  target_link_libraries(test_color_correct_effect PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_color_correct_effect PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_color_correct_effect PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_color_correct_effect COMMAND test_color_correct_effect)
endif()

# test_blur_effect pins the separable 3-tap box-blur math for
# BlurEffect (H + V directions). Same ME_WITH_GPU + Metal gating
# as test_color_correct_effect.
if(ME_WITH_GPU)
  add_executable(test_blur_effect test_main.cpp test_blur_effect.cpp)
  target_link_libraries(test_blur_effect PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_blur_effect PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_blur_effect PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_blur_effect COMMAND test_blur_effect)
endif()

# test_lut_effect pins 3D-LUT sampling (identity + inversion cubes)
# via pixel readback. ME_WITH_GPU-only; skips on non-Metal or on
# drivers without BGFX_CAPS_TEXTURE_3D.
if(ME_WITH_GPU)
  add_executable(test_lut_effect test_main.cpp test_lut_effect.cpp)
  target_link_libraries(test_lut_effect PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_lut_effect PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_lut_effect PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_lut_effect COMMAND test_lut_effect)
endif()

# test_effect_chain_compile pins GpuEffectChain::compile() fusion
# of adjacent color-correct effects into a single fused pass.
# ME_WITH_GPU-only; skips on non-Metal.
if(ME_WITH_GPU)
  add_executable(test_effect_chain_compile test_main.cpp test_effect_chain_compile.cpp)
  target_link_libraries(test_effect_chain_compile PRIVATE
    media_engine::media_engine
    doctest::doctest
    bgfx
    bx)
  target_include_directories(test_effect_chain_compile PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_effect_chain_compile PRIVATE ME_HAS_GPU=1)
  add_test(NAME test_effect_chain_compile COMMAND test_effect_chain_compile)
endif()

# test_animated_number_loader exercises the JSON → me::AnimatedNumber
# parse helper (layer 2 of transform-animated-support). Pure C++;
# pulls in nlohmann/json via loader_helpers.hpp.
target_include_directories(test_animated_number_loader PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_animated_number_loader PRIVATE nlohmann_json::nlohmann_json)

# test_compose_sink_gpu_detect exercises the
# me::orchestrator::is_gpu_compose_usable predicate with inline
# MockGpuBackend subclasses. Pure C++; no libav / bgfx. Needs
# src/ on the include path for compose_sink.hpp + gpu_backend.hpp.
target_include_directories(test_compose_sink_gpu_detect PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_render_thread exercises me::gpu::RenderThread — the MPSC-
# queue single-worker-thread class that BgfxGpuBackend uses to pin
# bgfx's API thread. Pure C++20 (std::thread + std::mutex + cv),
# no bgfx link — platform-agnostic, runs in every build.
target_include_directories(test_render_thread PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_lut_parser exercises the .cube file parser. Pure C++; no
# GPU dependency — runs in every build.
target_include_directories(test_lut_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_tempo exercises me::audio::TempoStretcher (SoundTouch
# wrapping). ME_HAS_SOUNDTOUCH compile def gates the real test
# body; without it the TU is empty and the binary runs as a
# 0-case doctest.
target_include_directories(test_tempo PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_SOUNDTOUCH)
  target_compile_definitions(test_tempo PRIVATE ME_HAS_SOUNDTOUCH=1)
endif()

# test_audio_effect_chain exercises the AudioEffect + AudioEffectChain
# + Gain/Pan/Lowpass concrete effects. Pure C++; no bgfx /
# SoundTouch / libav.
target_include_directories(test_audio_effect_chain PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_reencode_pts_remap exercises the PTS remap helper at the
# heart of the vfr-av-sync fix. Pure math on int64 + AVRational
# — needs libav headers (AVRational / av_rescale_q) but no
# codec contexts.
target_include_directories(test_reencode_pts_remap PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_reencode_pts_remap PRIVATE
  FFMPEG::avutil FFMPEG::avcodec)

# test_encode_hevc_main10 calls open_video_encoder directly via the
# detail:: namespace + needs libavcodec to allocate AVCodecContexts.
# Same shape as test_reencode_pts_remap above.
target_include_directories(test_encode_hevc_main10 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_encode_hevc_main10 PRIVATE
  FFMPEG::avutil FFMPEG::avcodec)
# M10 SW HEVC fallback dispatch (`hevc-sw` codec name) is gated on
# ME_HAS_KVAZAAR — same engine-level PRIVATE flag as
# test_kvazaar_hevc_encoder, so re-detect kvazaar here. With the
# macro defined, `'hevc-sw' preflight passes` runs end-to-end;
# without it, the without-ME_HAS_KVAZAAR diag branch runs instead.
if(PkgConfig_FOUND)
  pkg_check_modules(_TEST_HEVC_KVAZAAR QUIET kvazaar)
  if(_TEST_HEVC_KVAZAAR_FOUND)
    target_compile_definitions(test_encode_hevc_main10 PRIVATE
      ME_HAS_KVAZAAR=1)
  endif()
endif()

# test_tonemap_kernel exercises me::compose::apply_tonemap_inplace.
# Pure RGBA8 byte math — no libav, no fixtures. Needs src/ on
# include path for compose/tonemap_kernel.hpp.
target_include_directories(test_tonemap_kernel PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_inverse_tonemap_stub pins the deferred-impl contract for
# me::compose::apply_inverse_tonemap_inplace (registered kind +
# UNSUPPORTED short-circuit + buffer not mutated). Same shape as
# test_tonemap_kernel above — pure header-only consumer of the
# kernel TU; needs src/ for compose/inverse_tonemap_kernel.hpp.
target_include_directories(test_inverse_tonemap_stub PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_face_sticker_stub pins the registered-but-deferred contract
# for me::compose::apply_face_sticker_inplace PLUS the JSON loader
# round-trip for `kind: "face_sticker"` (M11 ml-effect-face-sticker-
# stub). Reaches into src/ for the kernel header and the timeline IR
# types; uses the schema-fixtures harness for the JSON cases (same
# pattern as test_timeline_schema_ml_asset).
target_include_directories(test_face_sticker_stub PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_effect_param_ml_asset_ref — coverage for the typed
# LandmarkAssetRef / MaskAssetRef schema (M11 exit criterion at
# MILESTONES.md:140). Loads JSON via the schema-fixtures harness;
# reaches into src/ for the effect_params/asset_ref.hpp header.
target_include_directories(test_effect_param_ml_asset_ref PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)

# test_face_mosaic_stub mirrors test_face_sticker_stub for the M11
# ml-effect-face-mosaic-stub bullet — argument-validation +
# UNSUPPORTED short-circuit + JSON loader round-trip for
# `kind: "face_mosaic"`.
target_include_directories(test_face_mosaic_stub PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_body_alpha_key_stub mirrors test_face_mosaic_stub for the
# M11 ml-effect-body-alpha-key-stub bullet — argument-validation
# + UNSUPPORTED short-circuit + JSON loader round-trip for
# `kind: "body_alpha_key"`. References a Mask asset (cycle 28's
# AssetKind::Mask) instead of Landmark.
target_include_directories(test_body_alpha_key_stub PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_subtitle_renderer exercises me::text::SubtitleRenderer
# (libass wrapper). ME_HAS_LIBASS compile def gates the real
# test body. Needs src/ on include path for the wrapper header.
target_include_directories(test_subtitle_renderer PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_LIBASS)
  target_compile_definitions(test_subtitle_renderer PRIVATE ME_HAS_LIBASS=1)
endif()

# test_skia_backend exercises me::text::SkiaBackend (Skia
# wrapper). ME_HAS_SKIA compile def gates the real test body.
target_include_directories(test_skia_backend PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_SKIA)
  target_compile_definitions(test_skia_backend PRIVATE ME_HAS_SKIA=1)
endif()

# test_text_renderer exercises me::text::TextRenderer
# (TextClipParams → canvas via SkiaBackend). Same ME_HAS_SKIA
# gate.
target_include_directories(test_text_renderer PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_SKIA)
  target_compile_definitions(test_text_renderer PRIVATE ME_HAS_SKIA=1)
endif()

# test_font_fallback pins SkiaBackend's draw_string_with_fallback
# — the CJK + emoji + matchFamilyStyleCharacter path that closes
# the M5 "CJK + emoji + 字体 fallback 正确" exit criterion.
target_include_directories(test_font_fallback PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_SKIA)
  target_compile_definitions(test_font_fallback PRIVATE ME_HAS_SKIA=1)
endif()

# test_frame_server reaches into resource/content_hash.hpp to compute
# the fixture's sha256 — needed by the invalidate-by-asset round-trip
# case (timeline JSON must seed contentHash so the side-index can map
# asset_hash → graph_hash). Pattern mirrors test_content_hash + the
# test_compose_* suites.
target_include_directories(test_frame_server PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_frame_server uses the deterministic fixture MP4 to verify
# the M6 frame-server path (me_render_frame → resolve_active_clip_at
# + DiskCache + compose_frame_at → RGBA8 me_frame). Pure C API
# consumption — no src/ include.
add_dependencies(test_frame_server determinism_fixture)
target_compile_definitions(test_frame_server PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_player drives me_player_t through play / pause / seek /
# audio-playhead-injection against the same determinism fixture as
# test_frame_server. Pure C API consumption — no src/ include.
# As of `debt-player-rate-audio-tempo`, the suite also exercises
# rate ≠ 1.0 over a real audio track via the with-audio fixture
# (sample-count drift bound proves SoundTouch tempo wiring works);
# both fixtures are built ahead of the test.
add_dependencies(test_player determinism_fixture
                              determinism_fixture_with_audio)
target_compile_definitions(test_player PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_disk_cache exercises DiskCache (filesystem-backed frame
# cache). Uses temp_directory_path for scratch. Needs src/ on
# include path for the header.
target_include_directories(test_disk_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)

# test_decode_kernels exercises the IoDemux + IoDecodeVideo +
# RenderConvertRgba8 kernels through scheduler.evaluate_port.
# First end-to-end use of the graph framework outside the
# Exporter's lazy-demux smoke. Reaches into src/ for the kernel
# registration helpers + graph/task/scheduler internals (the
# C API doesn't expose graph evaluation, by design). Uses the
# shared determinism fixture for a known 640×480 source.
target_include_directories(test_decode_kernels PRIVATE ${CMAKE_SOURCE_DIR}/src)
# scheduler.hpp transitively pulls <taskflow/taskflow.hpp> — Taskflow is
# PRIVATE-linked to media_engine, so test TUs need to see it explicitly
# (mirrors 02_graph_smoke's EXTRA_LIBS Taskflow).
target_link_libraries(test_decode_kernels PRIVATE Taskflow)
add_dependencies(test_decode_kernels determinism_fixture)
target_compile_definitions(test_decode_kernels PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_io_decode_audio exercises the IoDecodeAudio kernel against the
# audio-capable determinism fixture. Same Taskflow + src/ include
# pattern as test_decode_kernels.
target_include_directories(test_io_decode_audio PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_io_decode_audio PRIVATE Taskflow)
add_dependencies(test_io_decode_audio determinism_fixture_with_audio)
target_compile_definitions(test_io_decode_audio PRIVATE
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_audio_resample_kernel constructs synthetic AVFrames to drive
# the AudioResample kernel; needs libav for AVFrame construction.
target_include_directories(test_audio_resample_kernel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_resample_kernel PRIVATE
  FFMPEG::avutil FFMPEG::avcodec FFMPEG::swresample)

# test_audio_mix_kernel constructs synthetic FLTP AVFrames to drive
# the AudioMix kernel.
target_include_directories(test_audio_mix_kernel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_mix_kernel PRIVATE
  FFMPEG::avutil FFMPEG::avcodec)

# test_audio_timestretch_kernel — gated on ME_HAS_SOUNDTOUCH.
target_include_directories(test_audio_timestretch_kernel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_timestretch_kernel PRIVATE
  FFMPEG::avutil FFMPEG::avcodec)
if(ME_WITH_SOUNDTOUCH)
  target_compile_definitions(test_audio_timestretch_kernel PRIVATE ME_HAS_SOUNDTOUCH=1)
endif()

# test_audio_chunk_graph — end-to-end exercise of compile_audio_chunk_graph
# through the audio-capable determinism fixture. Same Taskflow + src/
# pattern as test_decode_kernels.
target_include_directories(test_audio_chunk_graph PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_chunk_graph PRIVATE
  Taskflow FFMPEG::avutil FFMPEG::avcodec)
add_dependencies(test_audio_chunk_graph determinism_fixture_with_audio)
target_compile_definitions(test_audio_chunk_graph PRIVATE
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_scheduler_cache exercises sched::OutputCache (peek-before-dispatch +
# bounded LRU). Uses mock kernels registered into the bootstrap test slots
# so the binary doesn't depend on engine-side kernel registration. Same
# src/ + Taskflow link as test_decode_kernels.
target_include_directories(test_scheduler_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_scheduler_cache PRIVATE Taskflow)
add_dependencies(test_audio_mixer determinism_fixture_with_audio)
target_compile_definitions(test_audio_mixer PRIVATE
  ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")

# test_frame_server_concurrent hammers me_render_frame from 4
# threads × 20 fetches on a shared engine + timeline to pin
# thread safety of the scrubbing path. Uses the determinism
# fixture (no audio needed).
add_dependencies(test_frame_server_concurrent determinism_fixture)
target_compile_definitions(test_frame_server_concurrent PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")

# test_compose_sink_e2e* — video core + audio + text + subtitle
# shards of the original 1136-line suite (split per
# debt-split-test-compose-sink-e2e-cpp). All four share fixtures
# via tests/compose_sink_e2e_fixtures.hpp and use the shared
# determinism fixture (audio shard additionally needs the
# with-audio variant for real AAC samples).
foreach(tgt
    test_compose_sink_e2e
    test_compose_sink_e2e_audio
    test_compose_sink_e2e_text
    test_compose_sink_e2e_subtitle)
  add_dependencies(${tgt} determinism_fixture determinism_fixture_with_audio)
  target_compile_definitions(${tgt} PRIVATE
    ME_TEST_FIXTURE_MP4="${_fixture_mp4}"
    ME_TEST_FIXTURE_MP4_WITH_AUDIO="${_fixture_mp4_with_audio}")
endforeach()

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

# ----- ctest coverage for examples 07 / 08 / 09 ---------------------------
# These three examples landed in cycle 65 (afa76a7) but no ctest target
# exercised them — a future me_render_frame / me_render_start /
# me_thumbnail signature drift would break them silently until talevia
# integrates. The add_test entries live here (not in examples/CMakeLists)
# because the determinism_fixture target is created above and top-level
# adds examples BEFORE tests; deferring to tests/ keeps the dependency
# direction one-way.
if(TARGET 07_compose_multitrack
    AND TARGET 08_frame_server_scrub
    AND TARGET 09_text_clip)
  set(_me_example_out "${CMAKE_BINARY_DIR}/examples/_test_output")
  file(MAKE_DIRECTORY "${_me_example_out}")

  add_test(NAME example_07_compose_multitrack
           COMMAND $<TARGET_FILE:07_compose_multitrack>
                   "${_fixture_mp4}"
                   "${_me_example_out}/07_out.mp4")
  set_tests_properties(example_07_compose_multitrack PROPERTIES TIMEOUT 30)
  add_dependencies(07_compose_multitrack determinism_fixture)

  # Companion check: verify the MP4 ftyp box at bytes 4-7 (matches
  # cycle 88's jni_passthrough_smoke_validate pattern). A regression
  # where the muxer flushed bytes that aren't a valid container
  # would slip past exit-code-only validation.
  add_test(NAME example_07_compose_multitrack_validate
           COMMAND bash -c
             "test \"$(dd if='${_me_example_out}/07_out.mp4' \
                           bs=1 skip=4 count=4 status=none)\" = ftyp")
  set_tests_properties(example_07_compose_multitrack_validate PROPERTIES
    DEPENDS example_07_compose_multitrack
    TIMEOUT 5)

  add_test(NAME example_08_frame_server_scrub
           COMMAND $<TARGET_FILE:08_frame_server_scrub>
                   "${_fixture_mp4}"
                   "${_me_example_out}/08_frames")
  set_tests_properties(example_08_frame_server_scrub PROPERTIES TIMEOUT 30)
  add_dependencies(08_frame_server_scrub determinism_fixture)

  # Companion check: the runner only verifies exit code. A regression
  # where me_render_frame returned ME_OK but the PPM write produced
  # zero bytes (or skipped frames) would slip through. Assert that
  # all 4 expected files exist with non-zero size. Sequential after
  # the runner via DEPENDS so the directory is populated.
  add_test(NAME example_08_frame_server_scrub_files
           COMMAND bash -c
             "test -s '${_me_example_out}/08_frames/frame_0.ppm' && \
              test -s '${_me_example_out}/08_frames/frame_1.ppm' && \
              test -s '${_me_example_out}/08_frames/frame_2.ppm' && \
              test -s '${_me_example_out}/08_frames/frame_3.ppm'")
  set_tests_properties(example_08_frame_server_scrub_files PROPERTIES
    DEPENDS example_08_frame_server_scrub
    TIMEOUT 5)

  # 09_text_clip composites a Skia-rendered text overlay onto video;
  # without ME_HAS_SKIA the text branch is omitted (compose still runs
  # the video bottom track). Run the test either way — exit code is the
  # signal we care about.
  add_test(NAME example_09_text_clip
           COMMAND $<TARGET_FILE:09_text_clip>
                   "${_fixture_mp4}"
                   "${_me_example_out}/09_out.mp4")
  set_tests_properties(example_09_text_clip PROPERTIES TIMEOUT 30)
  add_dependencies(09_text_clip determinism_fixture)

  # Companion check: same ftyp box validation as example_07's; the
  # text-overlay path goes through ComposeSink + h264 mux, so a
  # regression in either would still produce non-MP4 bytes.
  add_test(NAME example_09_text_clip_validate
           COMMAND bash -c
             "test \"$(dd if='${_me_example_out}/09_out.mp4' \
                           bs=1 skip=4 count=4 status=none)\" = ftyp")
  set_tests_properties(example_09_text_clip_validate PROPERTIES
    DEPENDS example_09_text_clip
    TIMEOUT 5)

  # 11_error_diagnosis: switch-on-status retry pattern. Hits 3
  # deliberate failure modes (parse / IO / unsupported) and exits
  # 0 only when all 3 expected statuses are observed. No fixture
  # needed — all 3 failures are self-contained.
  if(TARGET 11_error_diagnosis)
    add_test(NAME example_11_error_diagnosis
             COMMAND $<TARGET_FILE:11_error_diagnosis>)
    set_tests_properties(example_11_error_diagnosis PROPERTIES TIMEOUT 10)
  endif()

  # 12_cache_invalidate: cache-observability + invalidation demo.
  # The example asserts internally that AssetHashCache entries drop
  # across invalidate_asset AND that miss_count climbs both across
  # invalidate_asset (DiskCache side-index drops derived frames)
  # AND across cache_clear — exits non-zero if any of those fail.
  # The ctest's exit-code check below subsumes all three.
  if(TARGET 12_cache_invalidate)
    add_test(NAME example_12_cache_invalidate
             COMMAND $<TARGET_FILE:12_cache_invalidate> "${_fixture_mp4}")
    set_tests_properties(example_12_cache_invalidate PROPERTIES TIMEOUT 30)
    add_dependencies(12_cache_invalidate determinism_fixture)
  endif()
endif()

# ----- §5.7-5: libmedia_engine size budget guard ---------------------------
# Reads the size of the just-built static library and asserts ≤ the
# per-config ceiling in tools/lib_size_budget.txt. A new dependency
# or generated table that bloats the link graph fails this test
# rather than silently shipping inflated artifacts to host repos.
# Skipped on Windows (bash-only script); the budget file is platform-
# agnostic but the runner is not yet — Win-port is a follow-up.
if(NOT WIN32)
  # ME_WITH_INFERENCE selects an alternate budget row
  # (Release_with_inference, etc.) so the M11 ML runtime can pull in
  # CoreML / ONNX-runtime bytes without breaking the core-build
  # ceiling. ME_WITH_KVAZAAR composes a parallel `_with_kvazaar`
  # suffix for the M10 SW HEVC adapter; the two suffixes can stack
  # (`_with_inference_with_kvazaar`) so each combination has its own
  # ceiling. Single-config generators read CMAKE_BUILD_TYPE; multi-
  # config (Xcode) substitutes via $<CONFIG>. Suffix is empty when
  # neither option is set — same row as today.
  set(_lib_size_config_suffix "")
  if(ME_WITH_INFERENCE)
    string(APPEND _lib_size_config_suffix "_with_inference")
  endif()
  if(ME_WITH_KVAZAAR)
    string(APPEND _lib_size_config_suffix "_with_kvazaar")
  endif()
  add_test(NAME lib_size_budget
           COMMAND bash
                   "${CMAKE_SOURCE_DIR}/tools/check_lib_size.sh"
                   "$<TARGET_FILE:media_engine>"
                   "$<CONFIG>${_lib_size_config_suffix}"
                   "${CMAKE_SOURCE_DIR}/tools/lib_size_budget.txt")
  set_tests_properties(lib_size_budget PROPERTIES TIMEOUT 10)
endif()

# test_inference_coreml_skeleton — exercises me::inference::CoreMlRuntime's
# stub state. Reaches into src/inference/ for the runtime + coreml_runtime
# headers (private; the inference namespace is engine-internal). The
# ME_HAS_INFERENCE compile-def propagates from media_engine's PUBLIC
# target_compile_definitions, so the suite sees the same on/off as the
# engine itself.
target_include_directories(test_inference_coreml_skeleton PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)

# test_inference_multiarray_layout — pure C++ coverage for the
# strided→contiguous walk used by coreml_runtime.mm. Same src/-as-
# include pattern; no Apple frameworks needed.
target_include_directories(test_inference_multiarray_layout PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)

# test_inference_content_hash_cache — exercises me::inference::AssetCache
# (M11 exit criterion at MILESTONES.md:137, contentHash inference cache).
# ME_HAS_INFERENCE compile-def propagates from media_engine's PUBLIC
# target_compile_definitions, so the suite sees the same on/off as the
# engine itself. Pure C++ over the cache + a mock runtime — no Apple
# frameworks, no ONNX runtime.
target_include_directories(test_inference_content_hash_cache PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)

# test_inference_license_whitelist — exercises load_model_blob's
# license-whitelist + content-hash validation against a mock fetcher
# (M11 exit criterion at MILESTONES.md:138). Reaches into src/ for
# inference/model_loader.hpp + resource/content_hash.hpp; needs the
# real engine create/destroy to test fetcher registration end-to-end.
target_include_directories(test_inference_license_whitelist PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)

# test_inference_onnx_skeleton — exercises me::inference::OnnxRuntime's
# factory + lazy-session error path. ME_HAS_ONNX_RUNTIME is set as
# PRIVATE on the engine target (host SDKs shouldn't care which backend
# is linked) so we re-detect onnxruntime here via the same pkg-config
# probe and propagate the compile def only to the test source.
target_include_directories(test_inference_onnx_skeleton PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(_TEST_ONNX QUIET libonnxruntime)
  if(_TEST_ONNX_FOUND)
    target_compile_definitions(test_inference_onnx_skeleton PRIVATE
      ME_HAS_ONNX_RUNTIME=1)
  endif()
endif()

# test_kvazaar_hevc_encoder — exercises me::io::KvazaarHevcEncoder
# via synthetic gray YUV420P frames. ME_HAS_KVAZAAR is PRIVATE on
# the engine target; re-detect here so the test sees the same gate.
target_include_directories(test_kvazaar_hevc_encoder PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)
if(PkgConfig_FOUND)
  pkg_check_modules(_TEST_KVAZAAR QUIET kvazaar)
  if(_TEST_KVAZAAR_FOUND)
    target_compile_definitions(test_kvazaar_hevc_encoder PRIVATE
      ME_HAS_KVAZAAR=1)
  endif()
endif()

# test_hevc_sw_sink — drives HevcSwSink end-to-end with the
# determinism fixture (640x480 YUV420P, 25 frames). ME_HAS_KVAZAAR
# gates the integration TEST_CASE; the spec-matcher unit tests run
# without it. Same kvazaar re-detect pattern + adds avformat/avutil
# for the fixture-loading path.
target_include_directories(test_hevc_sw_sink PRIVATE
                           ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_hevc_sw_sink PRIVATE
  FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)
if(PkgConfig_FOUND)
  pkg_check_modules(_TEST_HEVC_SW_KVAZAAR QUIET kvazaar)
  if(_TEST_HEVC_SW_KVAZAAR_FOUND)
    target_compile_definitions(test_hevc_sw_sink PRIVATE
      ME_HAS_KVAZAAR=1
      ME_TEST_FIXTURE_MP4="${_fixture_mp4}")
    add_dependencies(test_hevc_sw_sink determinism_fixture)
  endif()
endif()

# ----- engine_impl.hpp ME_HAS_X visibility audit --------------------------
# Cycle 41 burned 7 cycles diagnosing a `pthread_mutex_lock EINVAL` flake
# whose root cause was `ME_HAS_SOUNDTOUCH` set as PRIVATE on media_engine.
# Because the macro gates a struct field in core/engine_impl.hpp, test TUs
# that include the header for white-box scheduler access saw a different
# me_engine layout than libmedia_engine.a — read `eng->scheduler` at the
# wrong offset → random vtable dispatch → corrupt mutex bytes.
#
# This test guards against the same trap recurring: any ME_HAS_X
# referenced in engine_impl.hpp MUST be PUBLIC or INTERFACE on the
# media_engine target so the macro propagates through
# target_link_libraries to consumers. PRIVATE silently breaks the test
# loop the next time someone adds a struct field guarded by a
# PRIVATE-scoped macro. The script's exit-2 message names the offending
# macro + line + remediation, so the failure is self-explanatory.
#
# No dependencies beyond bash + grep + awk — pure POSIX-shell static
# analysis of the two source files. Skipped on Windows (bash only) for
# the same reason as lib_size_budget.
if(NOT WIN32)
    add_test(NAME engine_impl_compile_defs_audit
             COMMAND bash
                     "${CMAKE_SOURCE_DIR}/tools/check-engine-impl-compile-defs.sh"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(engine_impl_compile_defs_audit PROPERTIES TIMEOUT 5)
endif()
