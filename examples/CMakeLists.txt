# Shared helper so every subdirectory CMakeLists collapses to a single line.
# Before this existed each of the 6 examples hand-rolled `add_executable +
# target_link_libraries` (plus a POST_BUILD copy for 01, plus a src/-include
# pair for 02 and 03) — 14 lines of boilerplate that grew by ~3 lines per
# new example. The function centralises the three decisions each example
# makes: which language (C vs C++), whether it reaches into `src/` internal
# headers, and whether it needs a companion data file copied next to the
# binary.
#
# Usage:
#   me_add_example(<name>
#     [LANG cpp]                      # default: C (main.c); cpp picks main.cpp
#     [INTERNAL]                      # reach into ${CMAKE_SOURCE_DIR}/src/
#     [EXTRA_LIBS target1 target2 …]  # extra PRIVATE link targets
#     [COPY_RESOURCE <filename>])     # POST_BUILD copy next to the binary
function(me_add_example name)
  cmake_parse_arguments(ARG "INTERNAL" "LANG;COPY_RESOURCE" "EXTRA_LIBS" ${ARGN})

  set(_src "main.c")
  if(ARG_LANG STREQUAL "cpp")
    set(_src "main.cpp")
  endif()

  add_executable(${name} ${_src})
  target_link_libraries(${name} PRIVATE media_engine::media_engine ${ARG_EXTRA_LIBS})

  if(ARG_INTERNAL)
    # Internal-validation examples reach into the engine's private headers
    # (graph / task / scheduler / …). That's intentional — they're not API
    # demos. Public demos must not need this flag.
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  endif()

  if(ARG_COPY_RESOURCE)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_COPY_RESOURCE}
              $<TARGET_FILE_DIR:${name}>/${ARG_COPY_RESOURCE})
  endif()
endfunction()

add_subdirectory(01_passthrough)
add_subdirectory(02_graph_smoke)
add_subdirectory(03_timeline_segments)
add_subdirectory(04_probe)
add_subdirectory(05_reencode)
add_subdirectory(06_thumbnail)
