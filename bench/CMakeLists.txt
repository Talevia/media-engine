# bench/ — GPU render benchmarks.
#
# Opt-in via ME_BUILD_BENCH=ON (default OFF). All targets here
# additionally require ME_WITH_GPU=ON — without bgfx linked, the
# benches can't measure anything useful.

if(NOT ME_WITH_GPU)
  message(STATUS "ME_BUILD_BENCH=ON but ME_WITH_GPU=OFF — no bench targets built.")
  return()
endif()

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
