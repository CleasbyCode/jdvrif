#!/usr/bin/env bash

# compile_jdvrif.sh — parallel incremental build wrapper for jdvrif

set -euo pipefail

CXX="${CXX:-g++}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
TARGET="${TARGET:-jdvrif}"
BUILD_MODE="${BUILD_MODE:-release}"
FORTIFY_LEVEL="${JDVRIF_FORTIFY_LEVEL:-3}"
SRCDIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

if [[ "$TARGET" == /* ]]; then
    TARGET_PATH="$TARGET"
else
    TARGET_PATH="$SRCDIR/$TARGET"
fi
TARGET_DIR="$(dirname -- "$TARGET_PATH")"
TARGET_NAME="$(basename -- "$TARGET_PATH")"

if [[ ! -d "$TARGET_DIR" ]]; then
    echo "Target directory does not exist: $TARGET_DIR" >&2
    exit 1
fi

case "$BUILD_MODE" in
    release|sanitize) ;;
    *)
        echo "Unknown BUILD_MODE: $BUILD_MODE" >&2
        exit 2
        ;;
esac

case "$FORTIFY_LEVEL" in
    2|3) ;;
    *)
        echo "Unsupported JDVRIF_FORTIFY_LEVEL: $FORTIFY_LEVEL (expected 2 or 3)." >&2
        exit 2
        ;;
esac

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required build tool: $1" >&2
        exit 1
    fi
}

need_cmd cmake
need_cmd flock
need_cmd "$CXX"
need_cmd "$LD"
need_cmd "$OBJCOPY"

CXX_PATH="$(command -v "$CXX")"
LD_PATH="$(command -v "$LD")"
OBJCOPY_PATH="$(command -v "$OBJCOPY")"

COMPILER_NAME="$(basename -- "$CXX")"
BUILD_KEY="${COMPILER_NAME}-${BUILD_MODE}-F${FORTIFY_LEVEL}"
BUILD_KEY="${BUILD_KEY//[^[:alnum:]._-]/_}"
BUILD_DIR="${JDVRIF_BUILD_DIR:-$SRCDIR/build/$BUILD_KEY}"

JOBS="${JDVRIF_JOBS:-}"
if [[ -z "$JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    else
        JOBS=4
    fi
    if (( JOBS > 8 )); then
        JOBS=8
    fi
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "JDVRIF_JOBS must be a positive integer." >&2
    exit 2
fi

cmake -E make_directory "$BUILD_DIR"
exec {BUILD_LOCK_FD}>"$BUILD_DIR/.jdvrif-build.lock"
flock "$BUILD_LOCK_FD"

GENERATOR_ARGS=()
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR_ARGS=(-G Ninja)
    elif command -v make >/dev/null 2>&1; then
        GENERATOR_ARGS=(-G "Unix Makefiles")
    else
        echo "Missing required build backend: install Ninja or Make." >&2
        exit 1
    fi
fi

echo "Compiling $TARGET_PATH ($BUILD_MODE, parallel incremental, $JOBS jobs)..."
env CXXFLAGS= LDFLAGS= cmake \
    "${GENERATOR_ARGS[@]}" \
    -S "$SRCDIR" \
    -B "$BUILD_DIR" \
    "-DCMAKE_CXX_COMPILER=$CXX_PATH" \
    "-DJDVRIF_BUILD_MODE=$BUILD_MODE" \
    "-DJDVRIF_FORTIFY_LEVEL=$FORTIFY_LEVEL" \
    "-DJDVRIF_LD=$LD_PATH" \
    "-DJDVRIF_OBJCOPY=$OBJCOPY_PATH"

cmake --build "$BUILD_DIR" --parallel "$JOBS" --target jdvrif

BUILT_BINARY="$BUILD_DIR/jdvrif"
if [[ ! -x "$BUILT_BINARY" ]]; then
    echo "Build Error: CMake did not produce an executable jdvrif binary." >&2
    exit 1
fi

if [[ ! -f "$TARGET_PATH" ]] || ! cmp -s -- "$BUILT_BINARY" "$TARGET_PATH"; then
    # Publish only a complete internal build. A configure, compile, or link
    # failure therefore leaves the previous public executable untouched.
    TEMP_TARGET="$(mktemp "$TARGET_DIR/.${TARGET_NAME}.tmp.XXXXXX")"
    trap 'rm -f -- "$TEMP_TARGET"' EXIT
    cp -- "$BUILT_BINARY" "$TEMP_TARGET"
    chmod --reference="$BUILT_BINARY" "$TEMP_TARGET"
    mv -f -- "$TEMP_TARGET" "$TARGET_PATH"
    trap - EXIT
fi

echo ""
echo "Compilation successful. Executable '$TARGET_PATH' is up to date."
echo ""
echo "Golden tests: bash \"$SRCDIR/tests/run_golden_tests.sh\" --bin \"$TARGET_PATH\""
echo "Round-trip tests: bash \"$SRCDIR/tests/run_roundtrip_tests.sh\" --bin \"$TARGET_PATH\""
echo "Security smoke tests: bash \"$SRCDIR/tests/run_security_smoke.sh\" --bin \"$TARGET_PATH\""
echo "Bluesky helper tests: bash \"$SRCDIR/tests/run_bsky_tests.sh\""
echo ""
