cmake_minimum_required(VERSION 3.20)

project(jdvrif VERSION 10.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "jdvrif requires Linux.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "jdvrif requires GCC 14 or newer.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "jdvrif requires Clang 18 or newer with libstdc++ 14 or newer.")
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

# The -x and -r carrier stages run on several threads, sized by the CPUs the
# process may use and the OMP_NUM_THREADS / OMP_THREAD_LIMIT settings. ON runs
# them on the calling thread alone.
option(JDVRIF_SERIAL_CARRIERS "Run the -x/-r carrier stages on one thread" OFF)

set(JDVRIF_FORTIFY_LEVEL "3" CACHE STRING "_FORTIFY_SOURCE level for release builds")
set_property(CACHE JDVRIF_FORTIFY_LEVEL PROPERTY STRINGS 2 3)
if(NOT JDVRIF_FORTIFY_LEVEL MATCHES "^[23]$")
  message(FATAL_ERROR "JDVRIF_FORTIFY_LEVEL must be 2 or 3.")
endif()

find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h REQUIRED)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg REQUIRED)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
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
# The templates are compiled in as C++ arrays (see embed_templates.cmake), so
# their object carries the same CET and non-executable-stack notes as the rest.
# Editing a template file re-runs this configure step.
include("${CMAKE_CURRENT_SOURCE_DIR}/embed_templates.cmake")
set(JDVRIF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
jdvrif_write_template_header(
  "${CMAKE_CURRENT_SOURCE_DIR}" "${JDVRIF_GENERATED_DIR}/template_assets_data.h" ${JDVRIF_TEMPLATE_FILES})
foreach(template IN LISTS JDVRIF_TEMPLATE_FILES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${template}")
endforeach()

add_executable(jdvrif
  binary_io.cpp
  file_utils.cpp
  template_assets.cpp
  jpeg_utils.cpp
  reddit_steg.cpp
  twitter_jpeg_codec.cpp
  twitter_juniward.cpp
  twitter_stc.cpp
  twitter_steg.cpp
  base64.cpp
  compression.cpp
  segmentation.cpp
  encryption_kdf.cpp
  encryption_stream.cpp
  encryption_stream_decrypt.cpp
  encryption.cpp
  encryption_bluesky.cpp
  key_codec.cpp
  key_input.cpp
  conceal.cpp
  recover_extract.cpp
  recover_output.cpp
  recover_modes.cpp
  recover.cpp
  program_args.cpp
  signal_utils.cpp
  main.cpp
)

target_include_directories(jdvrif PRIVATE
  "${JDVRIF_GENERATED_DIR}"
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
  JPEG::JPEG
  ZLIB::ZLIB
  "${LIBDEFLATE_LIBRARY}"
  "${SODIUM_LIBRARY}"
)

target_link_libraries(jdvrif PRIVATE Threads::Threads)
# Every mode, sanitizer builds included.
target_link_options(jdvrif PRIVATE -Wl,-z,noexecstack)
if(JDVRIF_SERIAL_CARRIERS)
  target_compile_definitions(jdvrif PRIVATE JDVRIF_SERIAL_CARRIERS)
endif()

if(JDVRIF_BUILD_MODE STREQUAL "release")
  target_compile_options(jdvrif PRIVATE
    -O3
    -march=native
    -pipe
    -fstack-protector-strong
    -fstack-clash-protection
    # Zero-fill locals the code leaves uninitialised, so a missed initialiser
    # reads zero rather than stale stack (KEY bytes included). Measured cost:
    # within noise.
    -ftrivial-auto-var-init=zero
    -fPIE
  )
  # Control-flow protection is per architecture: -fcf-protection is x86 only
  # (CET) and is an error elsewhere; arm64 has PAC/BTI.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_compile_options(jdvrif PRIVATE -fcf-protection=full)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(jdvrif PRIVATE -mbranch-protection=standard)
  endif()
  target_compile_definitions(jdvrif PRIVATE
    NDEBUG
    "_FORTIFY_SOURCE=${JDVRIF_FORTIFY_LEVEL}"
    # libstdc++ bounds checks (operator[], front/back, ...): an out-of-range
    # index aborts instead of reading or writing past a buffer. Measured at
    # about 2% on the -x/-r carrier stages and within noise elsewhere.
    _GLIBCXX_ASSERTIONS
  )
  target_link_options(jdvrif PRIVATE
    -s
    -pie
    -Wl,-z,relro,-z,now,-z,separate-code
  )
  # -fcf-protection=full marks each object as IBT and SHSTK ready, and the
  # linker keeps those marks only if every input object carries them. Where
  # the toolchain's own start files are marked (Ubuntu, Fedora), fail the link
  # instead of silently shipping without CET. Where they are not (Debian
  # 13: Scrt1.o, crti.o, crtn.o and libc itself), no executable can have CET
  # and the check would only break the build; tests/run_build_hardening_tests.sh
  # still checks that no jdvrif object is the one missing the marks.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fcf-protection=full")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,-z,cet-report=error")
    check_cxx_source_compiles("int main() { return 0; }" JDVRIF_TOOLCHAIN_HAS_CET)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(JDVRIF_TOOLCHAIN_HAS_CET)
      target_link_options(jdvrif PRIVATE -Wl,-z,cet-report=error)
    else()
      message(STATUS "Toolchain start files lack CET marks; the executable cannot carry IBT/SHSTK here.")
    endif()
  endif()

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
