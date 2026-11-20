# Fork: FFmpeg lookup for a plain prefix (include/, lib/*.lib, bin/*.dll), such as a BtbN
# shared build. vcpkg prefixes ship their own FindFFMPEG.cmake, which takes precedence.
# Sets FFMPEG_FOUND, FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARIES (the variables vcpkg's module sets).
set(_ffmpeg_root "${VEYRA_FFMPEG_ROOT}")
find_path(FFMPEG_INCLUDE_DIRS libavformat/avformat.h PATHS "${_ffmpeg_root}/include" NO_DEFAULT_PATH)
set(FFMPEG_LIBRARIES "")
foreach(_lib avformat avcodec avfilter avutil swresample swscale)
  find_library(FFMPEG_${_lib}_LIBRARY NAMES ${_lib} PATHS "${_ffmpeg_root}/lib" NO_DEFAULT_PATH)
  if(FFMPEG_${_lib}_LIBRARY)
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${_lib}_LIBRARY}")
  endif()
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFMPEG REQUIRED_VARS FFMPEG_INCLUDE_DIRS FFMPEG_avformat_LIBRARY FFMPEG_avcodec_LIBRARY FFMPEG_avutil_LIBRARY)
