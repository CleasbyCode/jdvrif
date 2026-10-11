#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-jpeg-safety.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

FLAGS=(-std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if [[ "${JDVRIF_TEST_SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
fi

"${CXX:-g++}" "${FLAGS[@]}" -I"$ROOT" \
    "$ROOT/tests/test_jpeg_safety.cpp" \
    "$ROOT/twitter_jpeg_codec.cpp" "$ROOT/reddit_steg.cpp" \
    "$ROOT/jpeg_utils.cpp" "$ROOT/signal_utils.cpp" \
    -Wl,--wrap=jpeg_CreateDecompress,--wrap=jpeg_CreateCompress \
    -Wl,--wrap=jpeg_destroy_decompress,--wrap=jpeg_destroy_compress \
    -Wl,--wrap=jpeg_read_coefficients,--wrap=jpeg_start_decompress \
    -ljpeg -lturbojpeg -lsodium -o "$WORK/test_jpeg_safety"

"$WORK/test_jpeg_safety"
