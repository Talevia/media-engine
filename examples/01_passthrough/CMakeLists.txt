add_executable(01_passthrough main.c)
target_link_libraries(01_passthrough PRIVATE media_engine::media_engine)

# Copy the sample JSON next to the binary for convenience.
add_custom_command(TARGET 01_passthrough POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/sample.timeline.json
          $<TARGET_FILE_DIR:01_passthrough>/sample.timeline.json)
