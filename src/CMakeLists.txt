include(FetchContent)
# Shared policy floor for FetchContent'd deps — see cmake/fetchcontent_policy.cmake.
# Harmless when every dep already meets the floor; cheap future-proofing against
# an nlohmann/taskflow release that bumps its cmake_minimum_required declaration
# below the current CMake-4.x cutoff.
include(fetchcontent_policy)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# Taskflow — header-only CPU work-stealing DAG (docs/ARCHITECTURE_GRAPH.md §scheduler).
FetchContent_Declare(
  taskflow
  GIT_REPOSITORY https://github.com/taskflow/taskflow.git
  GIT_TAG        v3.7.0
  GIT_SHALLOW    TRUE
)
set(TF_BUILD_TESTS      OFF CACHE INTERNAL "")
set(TF_BUILD_EXAMPLES   OFF CACHE INTERNAL "")
set(TF_BUILD_BENCHMARKS OFF CACHE INTERNAL "")
set(TF_BUILD_CUDA       OFF CACHE INTERNAL "")
set(TF_BUILD_SYCL       OFF CACHE INTERNAL "")
set(TF_BUILD_HIP        OFF CACHE INTERNAL "")
set(TF_BUILD_PROFILER   OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(taskflow)

find_package(FFMPEG REQUIRED)

# libass — M5 subtitle rendering via pkg-config find_package.
# Unlike the FetchContent'd deps (SoundTouch, bgfx, OCIO,
# nlohmann, taskflow), libass builds via autoconf + transitive
# non-CMake deps (fribidi, HarfBuzz, FreeType), so pkg-config
# on the dev machine's installed copy is the pragmatic path
# (see 6be4cde libass-subtitles-plan for the integration survey).
# Dev setup: `brew install libass` on macOS or `apt install
# libass-dev` on Linux. `ME_HAS_LIBASS=1` compile def gates the
# SubtitleRenderer codepath.
if(ME_WITH_LIBASS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBASS libass)
  if(NOT LIBASS_FOUND)
    message(WARNING "ME_WITH_LIBASS=ON but libass not found via "
            "pkg-config — flipping OFF for this build. Install via "
            "`brew install libass` (macOS) or `apt install libass-dev` "
            "(Linux) and reconfigure to enable subtitle rendering.")
    set(ME_WITH_LIBASS OFF CACHE BOOL "" FORCE)
  endif()
endif()

# Skia — prebuilt binary via JetBrains skia-pack (per ab08f7b
# skia-integration-plan). Platform coverage today: macOS arm64
# + macOS x86_64. The skia-pack release bundles per-platform
# artifacts; we pick the right one via the `SKIA_ARCH` branch
# below. Linux / Windows extensions add parallel branches in a
# follow-up cycle. Flag auto-flips OFF on unsupported hosts —
# SkiaBackend codepath degrades cleanly (ME_HAS_SKIA compile
# def gates every use).
if(ME_WITH_SKIA)
  set(SKIA_URL   "")
  set(SKIA_HASH  "")
  set(SKIA_ARCH  "")
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(SKIA_ARCH "arm64")
    set(SKIA_URL  "https://github.com/JetBrains/skia-pack/releases/download/m144-e2e6623374-4/Skia-m144-e2e6623374-4-macos-Release-arm64.zip")
    set(SKIA_HASH "5ec2f81f28bf79ce4b9fd744dd183b3f718cb72a742a8465ae44f395a7bfb411")
  elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(SKIA_ARCH "x64")
    set(SKIA_URL  "https://github.com/JetBrains/skia-pack/releases/download/m144-e2e6623374-4/Skia-m144-e2e6623374-4-macos-Release-x64.zip")
    set(SKIA_HASH "077513011356f9d96c596def3c5b292e43c705dc6902812daa82bc147303a9f4")
  else()
    message(WARNING "ME_WITH_SKIA=ON but platform is not macOS arm64 / x86_64 "
            "(got ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}) — "
            "flipping OFF. Linux / Windows binaries exist on the "
            "same skia-pack release; add parallel branches here "
            "when a real build target needs them.")
    set(ME_WITH_SKIA OFF CACHE BOOL "" FORCE)
  endif()

  if(ME_WITH_SKIA)
    FetchContent_Declare(
      skia_pack
      URL      "${SKIA_URL}"
      URL_HASH SHA256=${SKIA_HASH}
    )
    FetchContent_MakeAvailable(skia_pack)

    set(SKIA_OUT_DIR "${skia_pack_SOURCE_DIR}/out/Release-macos-${SKIA_ARCH}")
    add_library(skia STATIC IMPORTED)
    set_target_properties(skia PROPERTIES
      IMPORTED_LOCATION             "${SKIA_OUT_DIR}/libskia.a"
      INTERFACE_INCLUDE_DIRECTORIES "${skia_pack_SOURCE_DIR}"
    )
  endif()
endif()

# OpenColorIO — opt-in (off by default; see ME_WITH_OCIO in top-level
# CMakeLists.txt for the rationale). When on, we FetchContent OCIO
# v2.5.1 and let it pull its own transitive deps (Imath, yaml-cpp,
# pystring, expat, minizip-ng). v2.5.1 matters: upstream
# share/cmake/modules/install/Installyaml-cpp.cmake now passes
# `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` into its nested
# ExternalProject_Add for yaml-cpp 0.7.0, which is what unblocks the
# CMake-4.x floor + yaml-cpp's old `cmake_minimum_required(VERSION 3.4)`
# combo on our dev machine. 2.3.2 — our previous pin — lacked that
# flag and the build died at the yaml-cpp_install step.
# `me::color::OcioPipeline` lives next to `IdentityPipeline` in
# `src/color/` and links `OpenColorIO::OpenColorIO` conditionally.
if(ME_WITH_OCIO)
  FetchContent_Declare(
    OpenColorIO
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
    GIT_TAG        v2.5.1
    GIT_SHALLOW    TRUE
  )
  set(OCIO_BUILD_APPS       OFF CACHE INTERNAL "")
  set(OCIO_BUILD_TESTS      OFF CACHE INTERNAL "")
  set(OCIO_BUILD_GPU_TESTS  OFF CACHE INTERNAL "")
  set(OCIO_BUILD_DOCS       OFF CACHE INTERNAL "")
  set(OCIO_BUILD_PYTHON     OFF CACHE INTERNAL "")
  set(OCIO_BUILD_JAVA       OFF CACHE INTERNAL "")
  set(OCIO_WARNING_AS_ERROR OFF CACHE INTERNAL "")
  # OCIO's share/cmake/utils/CppVersion.cmake calls
  # `set_property(CACHE CMAKE_CXX_STANDARD PROPERTY STRINGS ...)`, which
  # requires CMAKE_CXX_STANDARD to be a CACHE variable. Our top-level
  # CMakeLists.txt sets it as a plain variable. Promote to CACHE before
  # MakeAvailable so OCIO's internal CMake finds it.
  set(CMAKE_CXX_STANDARD 20 CACHE STRING "" FORCE)
  include(fetchcontent_policy)
  FetchContent_MakeAvailable(OpenColorIO)
endif()

# bgfx — opt-in (off by default; see ME_WITH_GPU in top-level
# CMakeLists.txt). Fetched via `bkaradzic/bgfx.cmake` wrapper (bgfx
# itself uses GENie, not CMake). Version pin: v1.143.9226-530
# (tested on macOS arm64, 2026-04-23). Disable bgfx-side examples /
# tools / tests / shaderc build so first configure stays under
# several minutes and no GLFW / stb extras leak into link graph.
# `ME_HAS_GPU=1` compile def gates the BgfxGpuBackend codepath in
# `src/gpu/gpu_backend.cpp`; tests use the same define to guard
# ME_WITH_GPU-only assertions.
# SoundTouch — opt-in (ON by default; see ME_WITH_SOUNDTOUCH in
# top-level CMakeLists.txt). Fetched from codeberg at tag 2.4.0
# (verified 2026-04-24 via `git ls-remote --tags`). Disable
# SoundTouch-side options that don't serve our use case: the
# SoundStretch CLI, OpenMP (we run in a single-audio-thread
# model per-AudioMixer path), extra build warnings, install
# target. Consumer is `me::audio::TempoStretcher`.
if(ME_WITH_SOUNDTOUCH)
  FetchContent_Declare(
    soundtouch
    GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
    GIT_TAG        2.4.0
    GIT_SHALLOW    TRUE
  )
  set(SOUNDSTRETCH        OFF CACHE INTERNAL "")
  set(OPENMP              OFF CACHE INTERNAL "")
  set(INTEGER_SAMPLES     OFF CACHE INTERNAL "")
  include(fetchcontent_policy)
  FetchContent_MakeAvailable(soundtouch)

  # SoundTouch 2.4.0's PeakFinder.cpp trips
  # -Wunused-but-set-variable on clang — upstream 3rd-party code we
  # can't patch. Scope -Werror off for the SoundTouch target so
  # ME_WERROR=ON + ME_WITH_SOUNDTOUCH=ON is buildable. Our own
  # src/ / tests/ TUs keep -Werror.
  if(TARGET SoundTouch)
    target_compile_options(SoundTouch PRIVATE -Wno-error)
  endif()
endif()

if(ME_WITH_GPU)
  FetchContent_Declare(
    bgfx_cmake
    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG        v1.143.9226-530
    GIT_SHALLOW    TRUE
  )
  set(BGFX_BUILD_EXAMPLES  OFF CACHE INTERNAL "")
  set(BGFX_BUILD_TOOLS     OFF CACHE INTERNAL "")
  set(BGFX_BUILD_TESTS     OFF CACHE INTERNAL "")
  set(BGFX_INSTALL         OFF CACHE INTERNAL "")
  set(BGFX_CUSTOM_TARGETS  OFF CACHE INTERNAL "")
  include(fetchcontent_policy)
  FetchContent_MakeAvailable(bgfx_cmake)

  # bgfx + bx use GNU extensions (token-pasting `, ##__VA_ARGS__`,
  # variadic zero-args) that clang flags under
  # `-Wgnu-zero-variadic-macro-arguments`. Our root-level `-Werror`
  # (ME_WERROR=ON in CI / Release) would turn those into build
  # failures inside 3rd-party code we can't patch. Scope `-Werror`
  # off for the bgfx-side targets so `ME_WERROR=ON AND ME_WITH_GPU=ON`
  # is buildable. Our own `src/` / `tests/` TUs keep -Werror.
  foreach(_bgfx_tgt bgfx bx bimg bimg_decode bimg_encode astc-encoder edtaa3 etc1 etc2 iqa nvtt pvrtc squish)
    if(TARGET ${_bgfx_tgt})
      target_compile_options(${_bgfx_tgt} PRIVATE -Wno-error)
    endif()
  endforeach()
endif()

# --- Build-time version header --------------------------------------------
# Capture the git short SHA (+ "-dirty" if the working tree has uncommitted
# changes) at configure time and expose it to the C API via a generated
# header. Hosts use me_version().git_sha when filing bug reports to pin
# the exact build. Empty-string fallback when not in a git checkout.
set(ME_GIT_SHA "")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE ME_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_sha_rc
  )
  if(NOT _git_sha_rc EQUAL 0)
    set(ME_GIT_SHA "")
  endif()
  if(ME_GIT_SHA)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE _git_dirty
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(_git_dirty)
      set(ME_GIT_SHA "${ME_GIT_SHA}-dirty")
    endif()
  endif()
endif()
message(STATUS "media-engine git SHA: ${ME_GIT_SHA}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/core/version.inl.in
  ${CMAKE_CURRENT_BINARY_DIR}/core/version.inl
  @ONLY
)

# STATIC explicitly: without this the library follows BUILD_SHARED_LIBS,
# which OCIO's own CMakeLists defaults ON when we FetchContent it. Once
# flipped to shared, our hidden-by-default visibility would require
# explicit export annotations on every extern "C" API — work we don't
# want to take on just because a dep's defaults drift. STATIC sidesteps
# that entirely and matches the historical behavior of this library.
add_library(media_engine STATIC
  core/version.cpp
  core/engine_seed.cpp
  timeline/timeline_loader.cpp
  timeline/timeline_loader_assets.cpp
  timeline/timeline_loader_tracks.cpp
  timeline/timeline_loader_transitions.cpp
  timeline/loader_helpers_primitives.cpp
  timeline/loader_helpers_animated.cpp
  timeline/loader_helpers_clip_params.cpp
  timeline/animated_number.cpp
  timeline/animated_color.cpp
  timeline/segmentation.cpp
  gpu/gpu_backend.cpp
  gpu/render_thread.cpp
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/gpu/bgfx_gpu_backend.cpp>
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/effect/color_correct_effect.cpp>
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/effect/blur_effect.cpp>
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/effect/lut_effect.cpp>
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/effect/fused_color_correct_effect.cpp>
  $<$<BOOL:${ME_WITH_GPU}>:${CMAKE_CURRENT_SOURCE_DIR}/effect/gpu_effect_chain.cpp>
  effect/lut.cpp
  $<$<BOOL:${ME_WITH_SOUNDTOUCH}>:${CMAKE_CURRENT_SOURCE_DIR}/audio/tempo.cpp>
  $<$<BOOL:${ME_WITH_LIBASS}>:${CMAKE_CURRENT_SOURCE_DIR}/text/subtitle_renderer.cpp>
  $<$<BOOL:${ME_WITH_SKIA}>:${CMAKE_CURRENT_SOURCE_DIR}/text/skia_backend.cpp>
  $<$<BOOL:${ME_WITH_SKIA}>:${CMAKE_CURRENT_SOURCE_DIR}/text/text_renderer.cpp>
  color/pipeline.cpp
  audio/mix.cpp
  audio/mixer.cpp
  audio/resample.cpp
  audio/track_feed.cpp
  compose/active_clips.cpp
  compose/affine_blit.cpp
  compose/alpha_over.cpp
  compose/cross_dissolve.cpp
  compose/frame_convert.cpp
  $<$<BOOL:${ME_WITH_OCIO}>:${CMAKE_CURRENT_SOURCE_DIR}/color/ocio_pipeline.cpp>
  io/demux_context.cpp
  io/demux_kernel.cpp
  io/mux_context.cpp
  graph/graph.cpp
  task/registry.cpp
  resource/frame_pool.cpp
  resource/codec_pool.cpp
  resource/content_hash.cpp
  resource/asset_hash_cache.cpp
  resource/disk_cache.cpp
  scheduler/eval_instance.cpp
  scheduler/scheduler.cpp
  orchestrator/segment_cache.cpp
  orchestrator/muxer_state.cpp
  orchestrator/reencode_video.cpp
  orchestrator/reencode_audio.cpp
  orchestrator/reencode_segment.cpp
  orchestrator/reencode_pipeline.cpp
  orchestrator/output_sink.cpp
  orchestrator/compose_sink.cpp
  orchestrator/compose_decode_loop.cpp
  orchestrator/compose_transition_step.cpp
  orchestrator/compose_audio.cpp
  orchestrator/audio_only_sink.cpp
  orchestrator/encoder_mux_setup.cpp
  orchestrator/frame_puller.cpp
  orchestrator/exporter.cpp
  orchestrator/previewer.cpp
  orchestrator/composition_thumbnailer.cpp
  api/engine.cpp
  api/timeline.cpp
  api/render.cpp
  api/probe.cpp
  api/thumbnail.cpp
  api/cache.cpp
)
add_library(media_engine::media_engine ALIAS media_engine)

target_include_directories(media_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(media_engine
  PRIVATE
    nlohmann_json::nlohmann_json
    Taskflow
    FFMPEG::avformat
    FFMPEG::avcodec
    FFMPEG::avutil
    FFMPEG::swscale
    FFMPEG::swresample
)

# Conditional: `me::color::OcioPipeline` (future) will need OCIO linked.
# Done separately from the required-deps block so OFF builds stay clean.
if(ME_WITH_OCIO)
  # OCIO's in-tree target is the bare name `OpenColorIO` (no namespace).
  # A `find_package(OpenColorIO)` on an installed copy would expose
  # `OpenColorIO::OpenColorIO` — when that path lands, update here and
  # prefer the namespaced alias.
  target_link_libraries(media_engine PRIVATE OpenColorIO)
  target_compile_definitions(media_engine PRIVATE ME_HAS_OCIO=1)
endif()

# Conditional: `me::gpu::BgfxGpuBackend` links against bgfx (+ bx / bimg
# pulled in by the wrapper). `ME_HAS_GPU=1` compile def is the
# source-level switch mirroring OCIO's pattern.
if(ME_WITH_GPU)
  target_link_libraries(media_engine PRIVATE bgfx bx bimg)
  target_compile_definitions(media_engine PRIVATE ME_HAS_GPU=1)
endif()

# SoundTouch link — `me::audio::TempoStretcher` wraps SoundTouch's
# C++ API. ME_HAS_SOUNDTOUCH compile def mirrors the ME_HAS_OCIO /
# ME_HAS_GPU pattern.
if(ME_WITH_SOUNDTOUCH)
  target_link_libraries(media_engine PRIVATE SoundTouch)
  target_compile_definitions(media_engine PRIVATE ME_HAS_SOUNDTOUCH=1)
endif()

# libass link — SubtitleRenderer uses ass_library_init /
# ass_renderer_init / ass_read_memory / ass_render_frame from
# libass. pkg-config populated LIBASS_* vars above; imported
# target isn't used because pkg-config doesn't emit one
# directly.
if(ME_WITH_LIBASS)
  target_include_directories(media_engine PRIVATE ${LIBASS_INCLUDE_DIRS})
  target_link_directories(media_engine PRIVATE ${LIBASS_LIBRARY_DIRS})
  target_link_libraries(media_engine PRIVATE ${LIBASS_LIBRARIES})
  target_compile_definitions(media_engine PRIVATE ME_HAS_LIBASS=1)
endif()

# Skia link — SkiaBackend uses SkCanvas / SkFont / SkTextBlob.
# libskia.a bundles its transitive deps (FreeType / HarfBuzz /
# libpng / libjpeg / ...) so a single IMPORTED target covers
# linking, aside from macOS system frameworks Skia hooks (Core*).
if(ME_WITH_SKIA)
  target_link_libraries(media_engine PRIVATE
    skia
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework AppKit")
  target_compile_definitions(media_engine PRIVATE ME_HAS_SKIA=1)
endif()

target_compile_features(media_engine PUBLIC cxx_std_20)
set_target_properties(media_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
