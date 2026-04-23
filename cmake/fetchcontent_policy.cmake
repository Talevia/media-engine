# Shared policy workaround for FetchContent'd dependencies whose
# own CMakeLists declare a cmake_minimum_required() below the floor
# that recent CMake (4.x) enforces.
#
# Context: CMake >= 4.0 refuses to configure projects declaring
# cmake_minimum_required(VERSION <3.5). Some third-party deps we
# pull via FetchContent (e.g. doctest v2.4.11) still declare
# VERSION 2.8. Setting CMAKE_POLICY_VERSION_MINIMUM here lets those
# old declarations configure without individually patching the
# upstream. Once the upstream bumps, this is a no-op (setting a
# higher-than-needed minimum has no effect).
#
# Use: `include(fetchcontent_policy)` at directory scope *before*
# the first `FetchContent_MakeAvailable()` in that directory. The
# setting scopes to the including directory — adding a new
# FetchContent caller downstream does NOT automatically inherit,
# so each new call site that needs the workaround must include
# this file explicitly. Deliberate: forces the author to notice
# they're depending on an upstream that missed the floor.

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
