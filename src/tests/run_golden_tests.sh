#!/bin/bash
# Recover-only golden-file regression tests for jdvrif.
#
# Each golden/ case ships a pre-built embedded JPG, recovery PIN, and expected
# payload bytes. This catches regressions in recover, layout parsing, and
# format-specific extract paths without relying on fresh conceal RNG.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GOLDEN="$TESTS/golden"
MANIFEST="$GOLDEN/manifest.tsv"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_golden_tests.sh [options]

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

need_cmd python3
need_cmd cmp
need_cmd stat

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

if [[ ! -f "$MANIFEST" ]]; then
    echo "Missing golden manifest: $MANIFEST" >&2
    echo "Generate fixtures with: bash tests/generate_golden.sh" >&2
    exit 1
fi

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

assert_owner_only_permissions() {
    local path="$1"
    local tag="$2"
    local perms
    perms="$(stat -c '%a' "$path" 2>/dev/null || true)"
    if [[ "$perms" != "600" ]]; then
        echo "[FAIL] $tag: expected owner-only permissions (600), got ${perms:-unknown}" >&2
        return 1
    fi
    return 0
}

assert_icc_profile_signatures() {
    local image="$1"
    local tag="$2"
    if ! python3 - "$image" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
mntr = data.find(b"mntrRGB")
if mntr < 0:
    raise SystemExit("mntrRGB signature not found")
acsp = mntr + 24
if acsp + 4 > len(data):
    raise SystemExit("acsp range out of bounds")
if data[acsp:acsp + 4] != b"acsp":
    raise SystemExit(f"acsp signature mismatch at offset {acsp}: got={data[acsp:acsp+4]!r}")
PY
    then
        echo "[FAIL] $tag: ICC profile signature integrity check failed" >&2
        return 1
    fi
    return 0
}

assert_icc_segment_count() {
    local image="$1"
    local min_segments="$2"
    local tag="$3"
    local segment_total
    segment_total="$(python3 - "$image" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
sig = b"mntrRGB"
idx = data.find(sig)
if idx < 8:
    raise SystemExit("")
start = idx - 8
off = start + 0x2C8
if off + 2 > len(data):
    raise SystemExit("")
print(int.from_bytes(data[off:off + 2], "big"))
PY
)"
    if [[ -z "$segment_total" || "$segment_total" -lt "$min_segments" ]]; then
        echo "[FAIL] $tag: expected at least $min_segments ICC segments, got ${segment_total:-?}" >&2
        return 1
    fi
    return 0
}

assert_bluesky_layout() {
    local image="$1"
    local expect_pshop="$2"
    local expect_xmp="$3"
    local tag="$4"
    local has_pshop has_xmp
    has_pshop="$(python3 - "$image" <<'PY'
import sys
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
print(1 if b"Photoshop 3.0" in data else 0)
PY
)"
    has_xmp="$(python3 - "$image" <<'PY'
import sys
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
print(1 if b"<rdf:li" in data else 0)
PY
)"
    if [[ "$expect_pshop" -eq 1 && "$has_pshop" -ne 1 ]]; then
        echo "[FAIL] $tag: expected Photoshop segment" >&2
        return 1
    fi
    if [[ "$expect_pshop" -eq 0 && "$has_pshop" -ne 0 ]]; then
        echo "[FAIL] $tag: unexpected Photoshop segment" >&2
        return 1
    fi
    if [[ "$expect_xmp" -eq 1 && "$has_xmp" -ne 1 ]]; then
        echo "[FAIL] $tag: expected XMP base64 segment" >&2
        return 1
    fi
    if [[ "$expect_xmp" -eq 0 && "$has_xmp" -ne 0 ]]; then
        echo "[FAIL] $tag: unexpected XMP segment" >&2
        return 1
    fi
    return 0
}

assert_jdvrif_signature() {
    local image="$1"
    local tag="$2"
    if ! python3 - "$image" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
sig = bytes([0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9])
if data.find(sig) < 0:
    raise SystemExit("jdvrif signature not found")
PY
    then
        echo "[FAIL] $tag: jdvrif signature missing" >&2
        return 1
    fi
    return 0
}

PASS=0
FAIL=0

run_case() {
    local case_id="$1"
    local option="$2"
    local payload_rel="$3"
    local golden_rel="$4"
    local pin="$5"
    local min_icc="$6"
    local expect_pshop="$7"
    local expect_xmp="$8"

    local payload="$TESTS/$payload_rel"
    local golden="$TESTS/$golden_rel"
    local work="$TESTS/.work/$case_id"

    if [[ ! -f "$payload" ]]; then
        echo "[FAIL] $case_id: missing payload $payload_rel" >&2
        return 1
    fi
    if [[ ! -f "$golden" ]]; then
        echo "[FAIL] $case_id: missing golden image $golden_rel (run generate_golden.sh)" >&2
        return 1
    fi

    rm -rf "$work"
    mkdir -p "$work"
    cp "$golden" "$work/input.jpg"

    assert_jdvrif_signature "$golden" "$case_id" || return 1

    if [[ "$option" != "-b" ]]; then
        assert_icc_profile_signatures "$golden" "$case_id" || return 1
        if [[ "$min_icc" -gt 0 ]]; then
            assert_icc_segment_count "$golden" "$min_icc" "$case_id" || return 1
        fi
    else
        assert_bluesky_layout "$golden" "$expect_pshop" "$expect_xmp" "$case_id" || return 1
    fi

    pushd "$work" >/dev/null
    if ! printf '%s\n' "$pin" | "$BIN" recover input.jpg > recover.log 2>&1; then
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

    if ! assert_owner_only_permissions "$recovered" "$case_id"; then
        popd >/dev/null
        return 1
    fi

    if ! cmp -s "$recovered" "$payload"; then
        popd >/dev/null
        echo "[FAIL] $case_id: recovered bytes differ from golden payload" >&2
        return 1
    fi

    popd >/dev/null
    echo "[PASS] $case_id"
    return 0
}

mkdir -p "$TESTS/.work"
trap 'rm -rf "$TESTS/.work"' EXIT

while IFS=$'\t' read -r case_id option _cover_rel payload_rel golden_rel pin min_icc expect_pshop expect_xmp; do
    [[ -z "$case_id" || "$case_id" == "case_id" ]] && continue
    if [[ "$option" == "." ]]; then
        option=""
    fi
    if run_case "$case_id" "$option" "$payload_rel" "$golden_rel" "$pin" "$min_icc" "$expect_pshop" "$expect_xmp"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done < "$MANIFEST"

echo
echo "Golden test summary: PASS=$PASS FAIL=$FAIL"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
