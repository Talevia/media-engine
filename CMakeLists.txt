cmake_minimum_required(VERSION 3.24)

project(media_engine
  VERSION 0.0.1
  DESCRIPTION "Cross-platform media engine for AI-driven video creation"
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ME_BUILD_EXAMPLES "Build examples" ON)
option(ME_BUILD_TESTS    "Build unit tests" OFF)
option(ME_WERROR         "Treat warnings as errors" OFF)

# OCIO is the M2 compose-path color management library (see
# `docs/ARCHITECTURE.md` dependency table). Default ON as of the
# `ocio-pipeline-enable` cycle — OCIO v2.5.1 upstream now passes
# `CMAKE_POLICY_VERSION_MINIMUM=3.5` into its yaml-cpp_install
# ExternalProject, which unblocks the CMake-4.x floor issue that made
# 2.3.2 un-buildable on our dev machine. The compose kernel cycles
# (multi-track-compose-kernel, cross-dissolve-kernel) assume OCIO is
# linked and `make_pipeline()` returns `OcioPipeline` by default.
# Flip OFF to produce a color-unaware build (skips the ~5min first-
# configure cost at the expense of all color paths running through
# `IdentityPipeline`).
option(ME_WITH_OCIO      "Fetch and link OpenColorIO (M2 color pipeline)" ON)

# M4 audio time-stretch. When ON, fetches SoundTouch 2.4.0 and
# links `me::audio::TempoStretcher` (src/audio/tempo.{hpp,cpp})
# against it. Default ON — SoundTouch is LGPL-2.1 (white-listed
# in ARCHITECTURE.md) and compiles in seconds; non-audio-heavy
# workflows can flip OFF to skip the fetch.
option(ME_WITH_SOUNDTOUCH "Fetch and link SoundTouch (M4 tempo)"          ON)

# M3 GPU render benchmarks (bench/). OFF by default — benches are
# run ad-hoc by dev machines + CI perf gates, not on every build.
# Requires ME_WITH_GPU=ON to do anything useful; the bench/
# CMakeLists.txt detects and skips otherwise.
option(ME_BUILD_BENCH    "Build GPU render benchmarks (bench/)"          OFF)

# M3 GPU backend (bgfx). ON wires FetchContent_Declare(bgfx.cmake)
# and swaps the `make_gpu_backend()` factory to return a
# `BgfxGpuBackend` whose ctor performs a headless `bgfx::init`
# (Metal renderer preferred on macOS) + clears a 1x1 backbuffer +
# retains the context for future compose kernel cycles; dtor calls
# `bgfx::shutdown`. OFF keeps the phase-1 NullGpuBackend (CPU
# compose stays the only render path). Default OFF because bgfx
# adds ~several minutes first-configure cost and the compose
# kernels don't depend on it yet — it's live only for smoke /
# bring-up work until effect-gpu-* bullets land.
option(ME_WITH_GPU       "Enable bgfx GPU backend (M3 Metal on macOS)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
  if(ME_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

add_subdirectory(src)

if(ME_BUILD_EXAMPLES AND EXISTS ${CMAKE_SOURCE_DIR}/examples/CMakeLists.txt)
  add_subdirectory(examples)
endif()

if(ME_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ME_BUILD_BENCH)
  add_subdirectory(bench)
endif()
