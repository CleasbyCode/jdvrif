#!/bin/bash

# compile_jdvrif.sh — Release build script for jdvrif (multi-file layout)

set -euo pipefail

CXX="${CXX:-g++}"
TARGET="${TARGET:-jdvrif}"
SRCDIR="."

COMMON_WARNINGS="-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat -Wformat-security"
RELEASE_CXXFLAGS="-O3 -march=native -pipe -DNDEBUG -D_FORTIFY_SOURCE=3 -s -flto=auto -fuse-linker-plugin -fstack-protector-strong -fstack-clash-protection -fPIE"
RELEASE_LDFLAGS="-pie -Wl,-z,relro,-z,now"

CXXFLAGS="$COMMON_WARNINGS $RELEASE_CXXFLAGS"
LDFLAGS="$RELEASE_LDFLAGS -lturbojpeg -lz -lsodium"

SOURCES=(
    "$SRCDIR/binary_io.cpp"
    "$SRCDIR/file_utils.cpp"
    "$SRCDIR/jpeg_utils.cpp"
    "$SRCDIR/base64.cpp"
    "$SRCDIR/compression.cpp"
    "$SRCDIR/segmentation.cpp"
    "$SRCDIR/encryption_kdf.cpp"
    "$SRCDIR/encryption_stream.cpp"
    "$SRCDIR/encryption.cpp"
    "$SRCDIR/encryption_bluesky.cpp"
    "$SRCDIR/pin_input.cpp"
    "$SRCDIR/conceal.cpp"
    "$SRCDIR/recover.cpp"
    "$SRCDIR/recover_extract.cpp"
    "$SRCDIR/program_args.cpp"
    "$SRCDIR/main.cpp"
)

echo "Compiling $TARGET (release)..."
$CXX $CXXFLAGS "${SOURCES[@]}" $LDFLAGS -o "$TARGET"
echo "Compilation successful. Executable '$TARGET' created."
