#!/bin/bash

# compile_jdvrif.sh — Release build script for jdvrif (multi-file layout)

set -euo pipefail

CXX="${CXX:-g++}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
TARGET="${TARGET:-jdvrif}"
BUILD_MODE="${BUILD_MODE:-release}"
SRCDIR="."

CXX_VERSION="$($CXX --version 2>/dev/null || true)"
if [[ "$CXX_VERSION" == *clang* ]]; then
    LINKER_PLUGIN_FLAG=""
else
    LINKER_PLUGIN_FLAG="-fuse-linker-plugin"
fi

COMMON_WARNINGS="-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat -Wformat-security"
RELEASE_CXXFLAGS="-O3 -march=native -pipe -DNDEBUG -D_FORTIFY_SOURCE=3 -s -flto=auto $LINKER_PLUGIN_FLAG -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -fPIE"
RELEASE_LDFLAGS="-pie -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code"
SANITIZE_CXXFLAGS="-O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all -fstack-protector-strong -D_GLIBCXX_ASSERTIONS"
SANITIZE_LDFLAGS="-fsanitize=address,undefined"

case "$BUILD_MODE" in
    release)
        MODE_CXXFLAGS="$RELEASE_CXXFLAGS"
        MODE_LDFLAGS="$RELEASE_LDFLAGS"
        ;;
    sanitize)
        MODE_CXXFLAGS="$SANITIZE_CXXFLAGS"
        MODE_LDFLAGS="$SANITIZE_LDFLAGS"
        ;;
    *)
        echo "Unknown BUILD_MODE: $BUILD_MODE" >&2
        exit 2
        ;;
esac

CXXFLAGS="$COMMON_WARNINGS $MODE_CXXFLAGS"
LDFLAGS="$MODE_LDFLAGS -lturbojpeg -lz -ldeflate -lsodium"

TEMPLATE_OBJECT="$(mktemp "${TMPDIR:-/tmp}/jdvrif_template_assets.XXXXXX")"
trap 'rm -f "$TEMPLATE_OBJECT"' EXIT
TEMPLATE_FILES=(
    "templates/default_icc_template.bin"
    "templates/bluesky_exif_template.bin"
    "templates/photoshop_segment_template.bin"
    "templates/xmp_segment_template.bin"
)

SOURCES=(
    "$SRCDIR/binary_io.cpp"
    "$SRCDIR/file_utils.cpp"
    "$SRCDIR/template_assets.cpp"
    "$SRCDIR/jpeg_utils.cpp"
    "$SRCDIR/base64.cpp"
    "$SRCDIR/compression.cpp"
    "$SRCDIR/segmentation.cpp"
    "$SRCDIR/encryption_kdf.cpp"
    "$SRCDIR/encryption_stream.cpp"
    "$SRCDIR/encryption_stream_decrypt.cpp"
    "$SRCDIR/encryption.cpp"
    "$SRCDIR/encryption_bluesky.cpp"
    "$SRCDIR/pin_input.cpp"
    "$SRCDIR/conceal.cpp"
    "$SRCDIR/recover_extract.cpp"
    "$SRCDIR/recover_output.cpp"
    "$SRCDIR/recover_modes.cpp"
    "$SRCDIR/recover.cpp"
    "$SRCDIR/program_args.cpp"
    "$SRCDIR/main.cpp"
)

echo "Compiling $TARGET ($BUILD_MODE)..."
(cd "$SRCDIR" && "$LD" -r -b binary -o "$TEMPLATE_OBJECT" "${TEMPLATE_FILES[@]}")
if command -v "$OBJCOPY" >/dev/null 2>&1; then
    "$OBJCOPY" --rename-section .data=.rodata,alloc,load,readonly,data,contents "$TEMPLATE_OBJECT"
fi
$CXX $CXXFLAGS "${SOURCES[@]}" "$TEMPLATE_OBJECT" $LDFLAGS -o "$TARGET"
echo "Compilation successful. Executable '$TARGET' created."
echo "Golden tests: bash tests/run_golden_tests.sh"
