#!/bin/bash

# compile_jdvrif.sh — Release build script for jdvrif (multi-file layout)

set -euo pipefail

CXX="${CXX:-g++}"
TARGET="${TARGET:-jdvrif}"
BUILD_MODE="${BUILD_MODE:-release}"
SRCDIR="."

COMMON_WARNINGS="-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat -Wformat-security"
RELEASE_CXXFLAGS="-O3 -march=native -pipe -DNDEBUG -D_FORTIFY_SOURCE=3 -s -flto=auto -fuse-linker-plugin -fstack-protector-strong -fstack-clash-protection -fPIE"
RELEASE_LDFLAGS="-pie -Wl,-z,relro,-z,now"
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
LDFLAGS="$MODE_LDFLAGS -lturbojpeg -lz -lsodium"

SOURCES=(
    "$SRCDIR/binary_io.cpp"
    "$SRCDIR/file_utils.cpp"
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
    "$SRCDIR/recover.cpp"
    "$SRCDIR/recover_extract.cpp"
    "$SRCDIR/program_args.cpp"
    "$SRCDIR/main.cpp"
)

echo "Compiling $TARGET ($BUILD_MODE)..."
$CXX $CXXFLAGS "${SOURCES[@]}" $LDFLAGS -o "$TARGET"
echo "Compilation successful. Executable '$TARGET' created."
