#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-worker-team.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -pthread \
    -I"$ROOT" "$ROOT/tests/test_worker_team.cpp" \
    -Wl,--wrap=sched_getaffinity -o "$WORK/test_worker_team"
timeout 60 "$WORK/test_worker_team"
