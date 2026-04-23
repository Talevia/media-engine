include(FetchContent)

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

add_library(media_engine
  core/version.cpp
  timeline/timeline_loader.cpp
  timeline/segmentation.cpp
  io/demux_context.cpp
  io/demux_kernel.cpp
  graph/graph.cpp
  task/registry.cpp
  resource/frame_pool.cpp
  scheduler/eval_instance.cpp
  scheduler/scheduler.cpp
  orchestrator/segment_cache.cpp
  orchestrator/muxer_state.cpp
  orchestrator/exporter.cpp
  orchestrator/previewer.cpp
  orchestrator/thumbnailer.cpp
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
)

target_link_libraries(media_engine
  PRIVATE
    nlohmann_json::nlohmann_json
    Taskflow
    FFMPEG::avformat
    FFMPEG::avcodec
    FFMPEG::avutil
)

target_compile_features(media_engine PUBLIC cxx_std_20)
set_target_properties(media_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
