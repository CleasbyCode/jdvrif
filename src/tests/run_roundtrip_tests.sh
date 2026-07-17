#!/bin/bash
# Fresh conceal/recover round-trip regression tests for jdvrif.
#
# These complement the golden recover fixtures by exercising the current
# conceal pipeline, parsing the generated recovery PIN/output image, recovering,
# and comparing the recovered payload bytes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_roundtrip_tests.sh [options]

Options:
  --no-build   Reuse existing jdvrif binary.
  --bin <path> Use an explicit binary path.
  -h, --help   Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --bin)
            BIN="$2"
            NO_BUILD=1
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

need_cmd cmp
need_cmd sed

if [[ "$BIN" != /* ]]; then
    BIN="$(pwd -P)/${BIN#./}"
fi

if [[ "$NO_BUILD" -eq 0 ]]; then
    (cd "$ROOT" && bash ./compile_jdvrif.sh)
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found or not executable: $BIN" >&2
    exit 1
fi

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" JPG image: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

PASS=0
FAIL=0

run_case() {
    local case_id="$1"
    local option="$2"
    local cover_rel="$3"
    local payload_rel="$4"

    local cover="$TESTS/$cover_rel"
    local payload="$TESTS/$payload_rel"
    local work="$TESTS/.work_roundtrip/$case_id"

    if [[ ! -f "$cover" ]]; then
        echo "[FAIL] $case_id: missing cover $cover_rel" >&2
        return 1
    fi
    if [[ ! -f "$payload" ]]; then
        echo "[FAIL] $case_id: missing payload $payload_rel" >&2
        return 1
    fi

    rm -rf "$work"
    mkdir -p "$work"

    pushd "$work" >/dev/null
    if [[ -n "$option" ]]; then
        if ! "$BIN" conceal "$option" "$cover" "$payload" > conceal.log 2>&1; then
            popd >/dev/null
            echo "[FAIL] $case_id: conceal command failed" >&2
            cat "$work/conceal.log" >&2
            return 1
        fi
    else
        if ! "$BIN" conceal "$cover" "$payload" > conceal.log 2>&1; then
            popd >/dev/null
            echo "[FAIL] $case_id: conceal command failed" >&2
            cat "$work/conceal.log" >&2
            return 1
        fi
    fi

    local embedded
    local pin
    embedded="$(extract_embedded_image conceal.log)"
    pin="$(extract_pin conceal.log)"
    if [[ -z "$embedded" || -z "$pin" || ! -f "$embedded" ]]; then
        popd >/dev/null
        echo "[FAIL] $case_id: failed to parse conceal output" >&2
        cat "$work/conceal.log" >&2
        return 1
    fi

    if ! printf '%s\n' "$pin" | "$BIN" recover "$embedded" > recover.log 2>&1; then
        popd >/dev/null
        echo "[FAIL] $case_id: recover command failed" >&2
        cat "$work/recover.log" >&2
        return 1
    fi

    local recovered
    recovered="$(extract_recovered_file recover.log)"
    if [[ -z "$recovered" || ! -f "$recovered" ]]; then
        popd >/dev/null
        echo "[FAIL] $case_id: failed to parse recovered filename" >&2
        cat "$work/recover.log" >&2
        return 1
    fi

    if ! cmp -s "$recovered" "$payload"; then
        popd >/dev/null
        echo "[FAIL] $case_id: recovered bytes differ from source payload" >&2
        return 1
    fi

    popd >/dev/null
    echo "[PASS] $case_id"
    return 0
}

CASES=(
    $'default\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_text.txt'
    $'default_multiseg\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_multi.bin'
    $'default_space_name\t.\ttestdata/covers/cover_default.jpg\t.work_roundtrip/input_payloads/payload space.txt'
    $'reddit\t-r\ttestdata/covers/cover_reddit.jpg\ttestdata/payloads/payload_archive.zip'
    $'reddit_multiseg\t-r\ttestdata/covers/cover_reddit.jpg\ttestdata/payloads/payload_multi.bin'
    $'bluesky\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsingle.bin'
    $'bluesky_split\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsplit.bin'
    $'bluesky_xmp\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bxmp.bin'
)

mkdir -p "$TESTS/.work_roundtrip"
trap 'rm -rf "$TESTS/.work_roundtrip"' EXIT
mkdir -p "$TESTS/.work_roundtrip/input_payloads"
cp "$TESTS/testdata/payloads/payload_text.txt" \
    "$TESTS/.work_roundtrip/input_payloads/payload space.txt"

for row in "${CASES[@]}"; do
    IFS=$'\t' read -r case_id option cover_rel payload_rel <<<"$row"
    if [[ "$option" == "." ]]; then
        option=""
    fi
    if run_case "$case_id" "$option" "$cover_rel" "$payload_rel"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done

echo
echo "Round-trip test summary: PASS=$PASS FAIL=$FAIL"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
