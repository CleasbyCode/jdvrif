#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-codec-preparation.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
FLAGS=(-std=c++23 -O2 -Wall -Wextra -Wpedantic -pthread)
if [[ "${JDVRIF_TEST_SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
fi
"${CXX:-g++}" "${FLAGS[@]}" -I"$ROOT" \
    "$ROOT/tests/test_codec_preparation.cpp" "$ROOT/reddit_steg.cpp" \
    "$ROOT/twitter_steg.cpp" "$ROOT/twitter_jpeg_codec.cpp" \
    "$ROOT/twitter_juniward.cpp" "$ROOT/twitter_stc.cpp" "$ROOT/signal_utils.cpp" \
    -ljpeg -lsodium -o "$WORK/test_codec_preparation"
"$WORK/test_codec_preparation"
