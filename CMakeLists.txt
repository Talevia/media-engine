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
