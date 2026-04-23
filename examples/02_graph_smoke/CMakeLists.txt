add_executable(02_graph_smoke main.cpp)
target_link_libraries(02_graph_smoke PRIVATE media_engine Taskflow)
# Reaches into engine-internal headers; this is an internal validation tool,
# not a public API demo.
target_include_directories(02_graph_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
