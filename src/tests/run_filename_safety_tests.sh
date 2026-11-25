#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-filename-safety.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic \
    -I"$ROOT" "$ROOT/tests/test_filename_safety.cpp" "$ROOT/file_utils.cpp" \
    "$ROOT/recover_output.cpp" "$ROOT/signal_utils.cpp" -lsodium \
    -o "$WORK/test_filename_safety"
"$WORK/test_filename_safety"
