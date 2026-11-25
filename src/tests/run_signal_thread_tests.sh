#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-signal-threads.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -pthread \
    -I"$ROOT" "$ROOT/tests/test_signal_threads.cpp" "$ROOT/signal_utils.cpp" \
    -o "$WORK/test_signal_threads"
timeout 15s "$WORK/test_signal_threads"
