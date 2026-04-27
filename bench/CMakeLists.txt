# bench/ — media-engine benchmarks.
#
# Opt-in via ME_BUILD_BENCH=ON (default OFF). Individual targets
# below gate their own prerequisites (GPU / codec availability),
# so flipping ME_BUILD_BENCH=ON on a GPU-less machine still gives
# a useful subset of benches rather than nothing.

# bench_gpu_compose — 1080p@60fps 4-effect GPU chain bench.
# Needs ME_WITH_GPU=ON (bgfx linked); otherwise silently skipped.
if(ME_WITH_GPU)
  add_executable(bench_gpu_compose bench_gpu_compose.cpp)
  target_link_libraries(bench_gpu_compose PRIVATE
    media_engine::media_engine
    bgfx
    bx)
  target_include_directories(bench_gpu_compose PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(bench_gpu_compose PRIVATE ME_HAS_GPU=1)

  # Register as a ctest target when testing is also enabled. The
  # bench exits non-zero on budget miss, so a perf regression
  # surfaces as a red ctest run rather than requiring a manual
  # invocation. Only makes sense when ME_BUILD_TESTS=ON because
  # enable_testing() has to have fired first (see top-level CMake).
  if(ME_BUILD_TESTS)
    add_test(NAME bench_gpu_compose COMMAND bench_gpu_compose)
  endif()
else()
  message(STATUS "ME_BUILD_BENCH=ON but ME_WITH_GPU=OFF — skipping bench_gpu_compose.")
endif()

# Re-run find_package(FFMPEG) so the IMPORTED targets
# (FFMPEG::avformat etc.) are visible in this scope. src/ calls it
# too; the call is cheap when cached.
find_package(FFMPEG REQUIRED)

# bench_vfr_av_sync — end-to-end VFR A/V drift budget (1 ms / hour).
# Synthesises a short VFR MP4 via libav (MPEG-4 Part 2 video +
# AAC silent audio), runs reencode through me_render_start, reads
# last-packet PTS from the output streams, and asserts the scaled
# per-hour drift is under budget. Links against the engine's
# existing libav deps; no GPU needed. When encoders are missing
# (Linux CI minus VideoToolbox) the bench self-skips with exit 0.
add_executable(bench_vfr_av_sync bench_vfr_av_sync.cpp)
target_link_libraries(bench_vfr_av_sync PRIVATE
  media_engine::media_engine
  FFMPEG::avformat
  FFMPEG::avcodec
  FFMPEG::avutil
  FFMPEG::swscale
  FFMPEG::swresample)
target_include_directories(bench_vfr_av_sync PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(ME_BUILD_TESTS)
  add_test(NAME bench_vfr_av_sync COMMAND bench_vfr_av_sync)
  # Generous timeout — fixture synthesis + reencode should finish
  # in a few seconds; the cap catches a hang, not a slow machine.
  set_tests_properties(bench_vfr_av_sync PROPERTIES TIMEOUT 60)
endif()

# bench_text_paragraph — SkiaBackend::draw_paragraph throughput.
# Times a ~1000-codepoint CJK + emoji + Latin string rendered
# under max_width, reports avg fps, exits non-zero on budget miss
# (< 60 fps). Skips when ME_WITH_SKIA=OFF.
add_executable(bench_text_paragraph bench_text_paragraph.cpp)
target_link_libraries(bench_text_paragraph PRIVATE
  media_engine::media_engine)
target_include_directories(bench_text_paragraph PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ME_WITH_SKIA)
  target_compile_definitions(bench_text_paragraph PRIVATE ME_HAS_SKIA=1)
endif()

if(ME_BUILD_TESTS)
  add_test(NAME bench_text_paragraph COMMAND bench_text_paragraph)
  # RUN_SERIAL so ctest -jN doesn't co-schedule this perf bench
  # alongside other CPU-bound tests; shared-cache contention with
  # test_compose_sink_e2e / test_disk_cache on the same machine
  # otherwise halves measured fps.
  set_tests_properties(bench_text_paragraph PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE)
endif()

# bench_thumbnail_png — me_thumbnail_png (asset-level) throughput.
# Times PNG encode of a 160×120 thumbnail against the
# determinism_fixture MP4, asserts avg < 25 ms / frame.
add_executable(bench_thumbnail_png bench_thumbnail_png.cpp)
target_link_libraries(bench_thumbnail_png PRIVATE
  media_engine::media_engine)
target_include_directories(bench_thumbnail_png PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(ME_BUILD_TESTS)
  # Fixture path is set by tests/CMakeLists.txt; mirror the
  # construction rule so bench/ can reference it without
  # requiring tests/ to export it. The add_dependencies ensures
  # determinism_fixture runs before the bench.
  add_dependencies(bench_thumbnail_png determinism_fixture)
  set(_fixture_mp4 "${CMAKE_BINARY_DIR}/tests/fixtures/determinism_input.mp4")
  add_test(NAME bench_thumbnail_png
           COMMAND bench_thumbnail_png "${_fixture_mp4}")
  set_tests_properties(bench_thumbnail_png PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE)
endif()

# bench_tonemap_kernel — full-frame 4K HDR→SDR tonemap throughput.
# M10 added the apply_tonemap_inplace kernel (cycle 9); the
# bullet `bench-perf-budget-coverage-expand` flagged that no
# bench tracks its per-frame cost. Synthesises a 3840×2160
# RGBA8 gradient + iterates the kernel + asserts avg < 80 ms.
# No fixture — pure CPU byte math, self-contained. Reaches into
# src/ for the kernel header (compose/tonemap_kernel.hpp) and
# the typed params (timeline/timeline_ir_params.hpp).
add_executable(bench_tonemap_kernel bench_tonemap_kernel.cpp)
target_link_libraries(bench_tonemap_kernel PRIVATE
  media_engine::media_engine)
target_include_directories(bench_tonemap_kernel PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(ME_BUILD_TESTS)
  add_test(NAME bench_tonemap_kernel COMMAND bench_tonemap_kernel)
  set_tests_properties(bench_tonemap_kernel PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE)
endif()

# bench_hdr_roundtrip — M10 exit criterion 7 perf+correctness
# harness for HDR pipelines. Mode A: HDR fixture → passthrough
# render → re-probe → assert HDR side data round-trips. Mode B:
# render_frame → tonemap kernel → bounded-luminance correctness
# floor + alpha pass-through. Skips on hosts without
# hevc_videotoolbox (the gen_hdr_fixture target gracefully
# produces no fixture there; the bench skips on missing fixture).
add_executable(bench_hdr_roundtrip bench_hdr_roundtrip.cpp)
target_link_libraries(bench_hdr_roundtrip PRIVATE
  media_engine::media_engine)
target_include_directories(bench_hdr_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(ME_BUILD_TESTS)
  add_dependencies(bench_hdr_roundtrip hdr_fixture)
  set(_fixture_mp4_hdr "${CMAKE_BINARY_DIR}/tests/fixtures/hdr_input.mp4")
  add_test(NAME bench_hdr_roundtrip
           COMMAND bench_hdr_roundtrip "${_fixture_mp4_hdr}")
  set_tests_properties(bench_hdr_roundtrip PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE)
endif()
