cmake_minimum_required(VERSION 3.16)
project(silkcast_h264_pull LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  cpp-httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(cpp-httplib)

set(OPENH264_ROOT "" CACHE PATH "Path to OpenH264 installation prefix")

find_path(OPENH264_INCLUDE_DIR
  NAMES wels/codec_api.h
  HINTS
    ${OPENH264_ROOT}/include
    ${OPENH264_ROOT}
    /usr/include
    /usr/local/include
    /opt/homebrew/include
)

find_library(OPENH264_LIBRARY
  NAMES openh264
  HINTS
    ${OPENH264_ROOT}/lib
    ${OPENH264_ROOT}/lib64
    /usr/lib
    /usr/local/lib
    /opt/homebrew/lib
)

if(NOT OPENH264_INCLUDE_DIR OR NOT OPENH264_LIBRARY)
  message(FATAL_ERROR
    "OpenH264 not found. Install libopenh264 or set OPENH264_ROOT to a prefix "
    "containing include/wels/codec_api.h and lib/libopenh264.*")
endif()

add_executable(h264_pull
  h264_pull.cpp
)

target_include_directories(h264_pull PRIVATE
  ${cpp-httplib_SOURCE_DIR}
  ${OPENH264_INCLUDE_DIR}
)

target_link_libraries(h264_pull PRIVATE
  httplib::httplib
  ${OPENH264_LIBRARY}
)
