# Finds FFmpeg (libavformat / libavcodec / libavutil at minimum).
#
# Prefers pkg-config. Falls back to a manual search in common Homebrew / apt
# locations. Defines imported targets:
#
#   FFMPEG::avformat
#   FFMPEG::avcodec
#   FFMPEG::avutil
#
# Sets FFMPEG_FOUND on success.

find_package(PkgConfig QUIET)

set(_FFMPEG_REQUIRED_COMPONENTS avformat avcodec avutil)

set(FFMPEG_FOUND TRUE)

foreach(_comp ${_FFMPEG_REQUIRED_COMPONENTS})
  set(_pc_name "lib${_comp}")
  set(_libvar "FFMPEG_${_comp}_LIB")
  set(_incvar "FFMPEG_${_comp}_INC")

  if(PKG_CONFIG_FOUND)
    pkg_check_modules(${_pc_name} QUIET ${_pc_name})
  endif()

  if(${_pc_name}_FOUND)
    set(${_libvar} ${${_pc_name}_LINK_LIBRARIES})
    set(${_incvar} ${${_pc_name}_INCLUDE_DIRS})
  else()
    find_path(${_incvar}
      NAMES "lib${_comp}/${_comp}.h"
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(${_libvar}
      NAMES ${_comp}
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu)
  endif()

  if(NOT ${_libvar} OR NOT ${_incvar})
    set(FFMPEG_FOUND FALSE)
    message(STATUS "FFmpeg component ${_comp}: NOT FOUND")
  else()
    message(STATUS "FFmpeg ${_comp}: ${${_libvar}}  (headers: ${${_incvar}})")
    if(NOT TARGET FFMPEG::${_comp})
      add_library(FFMPEG::${_comp} UNKNOWN IMPORTED)
      set_target_properties(FFMPEG::${_comp} PROPERTIES
        IMPORTED_LOCATION "${${_libvar}}"
        INTERFACE_INCLUDE_DIRECTORIES "${${_incvar}}")
    endif()
  endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFMPEG
  REQUIRED_VARS FFMPEG_avformat_LIB FFMPEG_avcodec_LIB FFMPEG_avutil_LIB)
