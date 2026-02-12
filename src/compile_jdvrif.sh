#!/bin/bash

# compile_jdvrif.sh — Build script for jdvrif (multi-file layout)

set -e

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++23 -O3 -march=native -pipe -Wall -Wextra -Wpedantic -Wshadow -Wconversion -DNDEBUG -s -flto=auto -fuse-linker-plugin -fstack-protector-strong"
LDFLAGS="-lturbojpeg -lz -lsodium"
TARGET="jdvrif"
SRCDIR="."

SOURCES=(
    "$SRCDIR/binary_io.cpp"
    "$SRCDIR/file_utils.cpp"
    "$SRCDIR/jpeg_utils.cpp"
    "$SRCDIR/base64.cpp"
    "$SRCDIR/compression.cpp"
    "$SRCDIR/segmentation.cpp"
    "$SRCDIR/encryption.cpp"
    "$SRCDIR/pin_input.cpp"
    "$SRCDIR/conceal.cpp"
    "$SRCDIR/recover.cpp"
    "$SRCDIR/main.cpp"
)

echo "Compiling $TARGET..."
$CXX $CXXFLAGS "${SOURCES[@]}" $LDFLAGS -o "$TARGET"
echo "Compilation successful. Executable '$TARGET' created."
