include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

find_package(FFMPEG REQUIRED)

add_library(media_engine
  core/version.cpp
  timeline/timeline_loader.cpp
  io/ffmpeg_remux.cpp
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
