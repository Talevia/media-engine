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

# OpenColorIO — opt-in (off by default; see ME_WITH_OCIO in top-level
# CMakeLists.txt for the rationale). When on, we FetchContent OCIO
# 2.3.2 and let it pull its own transitive deps (Imath, yaml-cpp,
# pystring, expat, minizip-ng). OCIO's internal CMake sets
# `CMAKE_POLICY_VERSION_MINIMUM` via our shared helper so the CMake 4.x
# floor issue doesn't bite. `me::color::OcioPipeline` (not yet written)
# will live next to `IdentityPipeline` in `src/color/` and link
# `OpenColorIO::OpenColorIO` conditionally here.
if(ME_WITH_OCIO)
  FetchContent_Declare(
    OpenColorIO
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
    GIT_TAG        v2.3.2
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

add_library(media_engine
  core/version.cpp
  timeline/timeline_loader.cpp
  timeline/segmentation.cpp
  io/demux_context.cpp
  io/demux_kernel.cpp
  io/mux_context.cpp
  graph/graph.cpp
  task/registry.cpp
  resource/frame_pool.cpp
  resource/codec_pool.cpp
  resource/content_hash.cpp
  resource/asset_hash_cache.cpp
  scheduler/eval_instance.cpp
  scheduler/scheduler.cpp
  orchestrator/segment_cache.cpp
  orchestrator/muxer_state.cpp
  orchestrator/reencode_video.cpp
  orchestrator/reencode_audio.cpp
  orchestrator/reencode_pipeline.cpp
  orchestrator/output_sink.cpp
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

target_compile_features(media_engine PUBLIC cxx_std_20)
set_target_properties(media_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
