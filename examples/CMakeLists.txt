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

  # --- Argument validation ------------------------------------------------
  # Typo-tightened: before this block a caller writing
  # `me_add_example(foo INERNAL)` (missing 'T') would silently get a
  # non-INTERNAL build — the flag gets dropped into ARG_UNPARSED_ARGUMENTS
  # and nothing complains. Same for `LANG c` (c works semantically but the
  # helper only dispatches on "cpp"), or `COPY_RESOURCE sample.json`
  # when the file doesn't exist. Fail fast with a message that names the
  # offending input so configure errors are actionable.
  if(NOT name)
    message(FATAL_ERROR "me_add_example: missing target name (first positional argument)")
  endif()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "me_add_example(${name}): unknown option(s): ${ARG_UNPARSED_ARGUMENTS}. "
      "Valid options: [LANG cpp] [INTERNAL] [EXTRA_LIBS tgt...] [COPY_RESOURCE file]")
  endif()
  if(ARG_LANG AND NOT ARG_LANG STREQUAL "cpp")
    message(FATAL_ERROR
      "me_add_example(${name}): LANG='${ARG_LANG}' not supported. "
      "Use 'LANG cpp' or omit (defaults to C).")
  endif()
  if(ARG_COPY_RESOURCE AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_COPY_RESOURCE})
    message(FATAL_ERROR
      "me_add_example(${name}): COPY_RESOURCE='${ARG_COPY_RESOURCE}' does not exist "
      "at ${CMAKE_CURRENT_SOURCE_DIR}/")
  endif()

  set(_src "main.c")
  if(ARG_LANG STREQUAL "cpp")
    set(_src "main.cpp")
  endif()
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_src})
    message(FATAL_ERROR
      "me_add_example(${name}): source file '${_src}' does not exist at "
      "${CMAKE_CURRENT_SOURCE_DIR}/ (hint: LANG ${ARG_LANG} -> ${_src})")
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

# Expected example targets, kept in lock-step with the add_subdirectory
# calls below. The post-subdir foreach acts as a configure-time canary:
# if a subdir's CMakeLists is renamed or accidentally stops invoking
# me_add_example, the mismatch trips fatal_error at configure rather
# than surfacing later as a missing build target. Add new examples to
# both lists; keep order aligned.
set(_me_expected_examples
  01_passthrough
  02_graph_smoke
  03_timeline_segments
  04_probe
  05_reencode
  06_thumbnail
  07_compose_multitrack
  08_frame_server_scrub
  09_text_clip
  10_player_pause_seek
  11_error_diagnosis
  12_cache_invalidate
)

add_subdirectory(01_passthrough)
add_subdirectory(02_graph_smoke)
add_subdirectory(03_timeline_segments)
add_subdirectory(04_probe)
add_subdirectory(05_reencode)
add_subdirectory(06_thumbnail)
add_subdirectory(07_compose_multitrack)
add_subdirectory(08_frame_server_scrub)
add_subdirectory(09_text_clip)
add_subdirectory(10_player_pause_seek)
add_subdirectory(11_error_diagnosis)
add_subdirectory(12_cache_invalidate)

# --- Post-subdir self-check: every expected example target exists ---------
foreach(_expected ${_me_expected_examples})
  if(NOT TARGET ${_expected})
    message(FATAL_ERROR
      "examples/ self-check: expected target '${_expected}' was not "
      "created by its subdir's me_add_example() call. The most likely "
      "cause is an argument validation failure silently swallowed by a "
      "stale CMake cache — delete the build directory and reconfigure.")
  endif()
endforeach()

# --- Self-check: all expected examples link media_engine ------------------
# Every me_add_example() call links media_engine::media_engine; if a
# subdir's CMakeLists hand-rolls add_executable without going through
# the helper (which would skip the link and silently break the example
# at build time), this trips fatal_error at configure.
foreach(_expected ${_me_expected_examples})
  get_target_property(_libs ${_expected} LINK_LIBRARIES)
  if(NOT "media_engine::media_engine" IN_LIST _libs)
    message(FATAL_ERROR
      "examples/ self-check: target '${_expected}' does not link "
      "media_engine::media_engine. Did its CMakeLists bypass me_add_example?")
  endif()
endforeach()

# ctest coverage for examples 07 / 08 / 09 lives in tests/CMakeLists.txt
# (see "ctest coverage for examples 07/08/09" section). The fixture
# target (`determinism_fixture`) is created in tests/CMakeLists.txt and
# top-level adds tests AFTER examples, so the add_test calls have to
# live where the target is already in scope.
