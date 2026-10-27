#!/bin/bash
# Recover-only regression test for pre-v10 carrier images (decimal PINs).
#
# Recovers every tests/testdata/legacy_v9_0/*.jpg and legacy_v9_1/*.jpg with its
# .pin and compares the recovered file with the known payload using cmp.
#   v9.0 fixtures: expected payload is testdata/payloads/payload_text.txt
#   v9.1 fixtures: expected payload is testdata/legacy_v9_1/payload.bin
#     (reddit/twitter carriers plus default/bluesky, the KDF4 envelope images)
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# ROOT is the C++ source tree. This directory may sit inside it (src/tests)
# or beside it at the repository root (tests/).
ROOT="$(cd "$TESTS/.." && pwd -P)"
[[ -f "$ROOT/compile_jdvrif.sh" ]] || ROOT="$ROOT/src"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
WEB_OUT=""

usage() {
    cat <<'EOF2'
Usage: tests/run_legacy_fixture_tests.sh [options]

Options:
  --bin <path>      CLI binary (jdvrif or jdvrif-rs); PIN is piped on stdin.
  --web-out <path>  Web recover binary (jdv_out); PIN is passed as pin.txt.
  -h, --help        Show this help.
EOF2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --web-out) WEB_OUT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

abspath() { [[ "$1" == /* ]] && printf '%s' "$1" || printf '%s/%s' "$(pwd -P)" "${1#./}"; }
if [[ -n "$WEB_OUT" ]]; then
    WEB_OUT="$(abspath "$WEB_OUT")"
    [[ -x "$WEB_OUT" ]] || { echo "Web binary not found or not executable: $WEB_OUT" >&2; exit 1; }
else
    BIN="$(abspath "$BIN")"
    [[ -x "$BIN" ]] || { echo "Binary not found or not executable: $BIN" >&2; exit 1; }
fi
command -v cmp >/dev/null || { echo "Missing required command: cmp" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

PASS=0
FAIL=0

# check_case <label> <image> <pin file> <expected payload>
check_case() {
    local label="$1" image="$2" pinfile="$3" expected="$4"
    local dir="$WORK/$label"
    mkdir -p "$dir"
    cp "$image" "$dir/embedded.jpg"
    if [[ -n "$WEB_OUT" ]]; then
        cp "$pinfile" "$dir/pin.txt"
        (cd "$dir" && "$WEB_OUT" embedded.jpg pin.txt > recover.log 2>&1) || true
    else
        (cd "$dir" && "$BIN" recover embedded.jpg < "$pinfile" > recover.log 2>&1) || true
    fi
    local out="" f
    for f in "$dir"/*; do
        case "$(basename "$f")" in
            embedded.jpg|pin.txt|recover.log) ;;
            *) out="$f"; break ;;
        esac
    done
    if [[ -n "$out" ]] && cmp -s "$expected" "$out"; then
        echo "[PASS] $label"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] $label: recovered file missing or differs from $(basename "$expected")" >&2
        sed 's/^/    /' "$dir/recover.log" >&2 || true
        FAIL=$((FAIL + 1))
    fi
}

V90="$TESTS/testdata/legacy_v9_0"
V91="$TESTS/testdata/legacy_v9_1"

for img in "$V90"/*.jpg; do
    n="$(basename "$img" .jpg)"
    check_case "v9.0_$n" "$img" "$V90/$n.pin" "$TESTS/testdata/payloads/payload_text.txt"
done
for img in "$V91"/*.jpg; do
    n="$(basename "$img" .jpg)"
    check_case "v9.1_$n" "$img" "$V91/$n.pin" "$V91/payload.bin"
done

echo "legacy fixtures: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 && "$PASS" -gt 0 ]]
