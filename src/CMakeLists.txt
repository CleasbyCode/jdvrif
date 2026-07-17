cmake_minimum_required(VERSION 3.20)

project(jdvrif VERSION 8.1 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "jdvrif requires Linux.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "jdvrif requires GCC 14 or newer.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "jdvrif requires Clang 18 or newer with a compatible C++23 standard library.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(JDVRIF_BUILD_MODE "release" CACHE STRING "jdvrif build mode")
set_property(CACHE JDVRIF_BUILD_MODE PROPERTY STRINGS release sanitize)
if(NOT JDVRIF_BUILD_MODE MATCHES "^(release|sanitize)$")
  message(FATAL_ERROR "JDVRIF_BUILD_MODE must be release or sanitize.")
endif()

set(JDVRIF_FORTIFY_LEVEL "3" CACHE STRING "_FORTIFY_SOURCE level for release builds")
set_property(CACHE JDVRIF_FORTIFY_LEVEL PROPERTY STRINGS 2 3)
if(NOT JDVRIF_FORTIFY_LEVEL MATCHES "^[23]$")
  message(FATAL_ERROR "JDVRIF_FORTIFY_LEVEL must be 2 or 3.")
endif()

set(JDVRIF_LD "${CMAKE_LINKER}" CACHE FILEPATH "Linker used to embed binary templates")
set(JDVRIF_OBJCOPY "${CMAKE_OBJCOPY}" CACHE FILEPATH "objcopy used to make embedded templates read-only")
if(NOT EXISTS "${JDVRIF_LD}")
  message(FATAL_ERROR "Required linker not found: ${JDVRIF_LD}")
endif()
if(NOT EXISTS "${JDVRIF_OBJCOPY}")
  message(FATAL_ERROR "Required objcopy not found: ${JDVRIF_OBJCOPY}")
endif()

find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h REQUIRED)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg REQUIRED)
find_package(ZLIB REQUIRED)
find_path(LIBDEFLATE_INCLUDE_DIR libdeflate.h REQUIRED)
find_library(LIBDEFLATE_LIBRARY NAMES deflate REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium REQUIRED)

set(JDVRIF_TEMPLATE_FILES
  templates/default_icc_template.bin
  templates/bluesky_exif_template.bin
  templates/photoshop_segment_template.bin
  templates/xmp_segment_template.bin
)
set(JDVRIF_TEMPLATE_DEPENDS)
foreach(template IN LISTS JDVRIF_TEMPLATE_FILES)
  list(APPEND JDVRIF_TEMPLATE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${template}")
endforeach()

set(JDVRIF_TEMPLATE_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/jdvrif_template_assets.o")
add_custom_command(
  OUTPUT "${JDVRIF_TEMPLATE_OBJECT}"
  COMMAND "${JDVRIF_LD}" -r -b binary -o "${JDVRIF_TEMPLATE_OBJECT}" ${JDVRIF_TEMPLATE_FILES}
  COMMAND "${JDVRIF_OBJCOPY}"
    --rename-section .data=.rodata,alloc,load,readonly,data,contents
    "${JDVRIF_TEMPLATE_OBJECT}"
  DEPENDS ${JDVRIF_TEMPLATE_DEPENDS}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Embedding jdvrif template assets"
  VERBATIM
)
set_source_files_properties("${JDVRIF_TEMPLATE_OBJECT}" PROPERTIES
  GENERATED TRUE
  EXTERNAL_OBJECT TRUE
)

add_executable(jdvrif
  binary_io.cpp
  file_utils.cpp
  template_assets.cpp
  jpeg_utils.cpp
  base64.cpp
  compression.cpp
  segmentation.cpp
  encryption_kdf.cpp
  encryption_stream.cpp
  encryption_stream_decrypt.cpp
  encryption.cpp
  encryption_bluesky.cpp
  pin_input.cpp
  conceal.cpp
  recover_extract.cpp
  recover_output.cpp
  recover_modes.cpp
  recover.cpp
  program_args.cpp
  signal_utils.cpp
  main.cpp
  "${JDVRIF_TEMPLATE_OBJECT}"
)

target_include_directories(jdvrif PRIVATE
  "${TURBOJPEG_INCLUDE_DIR}"
  "${LIBDEFLATE_INCLUDE_DIR}"
  "${SODIUM_INCLUDE_DIR}"
)

target_compile_options(jdvrif PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wformat
  -Wformat-security
)

target_link_libraries(jdvrif PRIVATE
  "${TURBOJPEG_LIBRARY}"
  ZLIB::ZLIB
  "${LIBDEFLATE_LIBRARY}"
  "${SODIUM_LIBRARY}"
)

if(JDVRIF_BUILD_MODE STREQUAL "release")
  target_compile_options(jdvrif PRIVATE
    -O3
    -march=native
    -pipe
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection=full
    -fPIE
  )
  target_compile_definitions(jdvrif PRIVATE
    NDEBUG
    "_FORTIFY_SOURCE=${JDVRIF_FORTIFY_LEVEL}"
  )
  target_link_options(jdvrif PRIVATE
    -s
    -pie
    -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code
  )

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(jdvrif PRIVATE -fuse-linker-plugin)
    target_link_options(jdvrif PRIVATE -fuse-linker-plugin)
  endif()

  include(CheckIPOSupported)
  check_ipo_supported(RESULT JDVRIF_IPO_SUPPORTED OUTPUT JDVRIF_IPO_ERROR LANGUAGES CXX)
  if(NOT JDVRIF_IPO_SUPPORTED)
    message(FATAL_ERROR "The selected compiler does not support LTO: ${JDVRIF_IPO_ERROR}")
  endif()
  set_property(TARGET jdvrif PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  target_compile_options(jdvrif PRIVATE
    -O1
    -g3
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fstack-protector-strong
  )
  target_compile_definitions(jdvrif PRIVATE _GLIBCXX_ASSERTIONS)
  target_link_options(jdvrif PRIVATE -fsanitize=address,undefined)
endif()
