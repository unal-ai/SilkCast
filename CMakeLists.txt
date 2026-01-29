cmake_minimum_required(VERSION 3.16)
project(SilkCast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
include(CheckIncludeFileCXX)
include(CheckSymbolExists)

FetchContent_Declare(
  cpp-httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)

FetchContent_MakeAvailable(cpp-httplib)

option(ENABLE_OPENH264 "Enable OpenH264 for H.264 baseline encoding" ON)
option(AUTO_FETCH_OPENH264 "Automatically download OpenH264 v2.6.0 binary+headers on Linux x86_64/arm64" ON)
set(OPENH264_ROOT "" CACHE PATH "Path to OpenH264 install prefix or unpacked binary")

if(ENABLE_OPENH264)
  if(AUTO_FETCH_OPENH264 AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64"))
    set(_OH_VERSION 2.6.0)
    set(_OH_BASE ${CMAKE_BINARY_DIR}/third_party/openh264)
    file(MAKE_DIRECTORY ${_OH_BASE}/lib)
    # Headers via FetchContent
    FetchContent_Declare(
      openh264_src
      URL https://github.com/cisco/openh264/archive/refs/tags/v${_OH_VERSION}.tar.gz
    )
    FetchContent_MakeAvailable(openh264_src)
    # OpenH264 headers are in codec/api/ subdirectory (not include/)
    set(OPENH264_INCLUDE_DIR ${openh264_src_SOURCE_DIR}/codec/api)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
      set(_OH_BIN_URL http://ciscobinary.openh264.org/libopenh264-${_OH_VERSION}-linux64.8.so.bz2)
    else()
      set(_OH_BIN_URL http://ciscobinary.openh264.org/libopenh264-${_OH_VERSION}-linux-arm64.8.so.bz2)
    endif()
    set(_OH_BIN_BZ2 ${_OH_BASE}/lib/libopenh264.so.bz2)
    set(_OH_BIN ${_OH_BASE}/lib/libopenh264.so)
    if(NOT EXISTS ${_OH_BIN})
      message(STATUS "Fetching OpenH264 binary from ${_OH_BIN_URL}")
      file(MAKE_DIRECTORY ${_OH_BASE}/lib)
      file(DOWNLOAD ${_OH_BIN_URL} ${_OH_BIN_BZ2} SHOW_PROGRESS)
      # The .bz2 file is just bz2-compressed (not a tarball), use bunzip2
      find_program(BUNZIP2 bunzip2)
      if(BUNZIP2)
        execute_process(COMMAND ${BUNZIP2} -f ${_OH_BIN_BZ2} WORKING_DIRECTORY ${_OH_BASE}/lib)
        # bunzip2 -f removes .bz2 suffix and overwrites, output is libopenh264.so
      else()
        # Fallback: download directly as .so (without bz2)
        message(WARNING "bunzip2 not found, trying alternative download")
      endif()
      file(DOWNLOAD http://www.openh264.org/BINARY_LICENSE.txt ${_OH_BASE}/BINARY_LICENSE.txt SHOW_PROGRESS)
    endif()
    set(OPENH264_LIBRARY ${_OH_BIN})
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(OPENH264 QUIET openh264)
    endif()

    find_path(OPENH264_INCLUDE_DIR
      NAMES wels/codec_api.h codec/api/svc/codec_api.h
      HINTS ${OPENH264_INCLUDE_DIRS} ${OPENH264_ROOT}/include ${OPENH264_ROOT}
            /usr/include /usr/local/include
    )
    find_library(OPENH264_LIBRARY
      NAMES openh264
      HINTS ${OPENH264_LIBRARY_DIRS} ${OPENH264_ROOT}/lib ${OPENH264_ROOT}/lib64
            /usr/lib /usr/local/lib
    )
  endif()

  if(OPENH264_INCLUDE_DIR AND OPENH264_LIBRARY)
    message(STATUS "Found OpenH264: ${OPENH264_LIBRARY}")
    set(HAS_OPENH264 TRUE)
  else()
    message(WARNING "OpenH264 not found; H.264 endpoint will fall back to MJPEG placeholder. Set OPENH264_ROOT or install libopenh264.")
    set(HAS_OPENH264 FALSE)
  endif()
else()
  set(HAS_OPENH264 FALSE)
endif()

set(SILKCAST_SOURCES
  src/main.cpp
  src/types.hpp
  src/capture_v4l2.cpp
  src/capture_v4l2.hpp
  src/encoder_h264.cpp
  src/encoder_h264.hpp
  src/mp4_frag.cpp
  src/mp4_frag.hpp
  src/yuv_convert.hpp
)

if(APPLE)
  list(APPEND SILKCAST_SOURCES src/capture_avfoundation.mm)
  set_source_files_properties(src/capture_avfoundation.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

add_executable(silkcast ${SILKCAST_SOURCES})

target_include_directories(silkcast PRIVATE ${cpp-httplib_SOURCE_DIR})
target_link_libraries(silkcast PRIVATE httplib::httplib pthread)

if(HAS_OPENH264)
  target_compile_definitions(silkcast PRIVATE HAS_OPENH264=1)
  target_include_directories(silkcast PRIVATE ${OPENH264_INCLUDE_DIR})
  target_link_libraries(silkcast PRIVATE ${OPENH264_LIBRARY})
endif()

if(APPLE)
  target_link_libraries(silkcast PRIVATE
    "-framework AVFoundation"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreImage"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework ImageIO"
    "-framework SystemConfiguration"
  )
endif()
