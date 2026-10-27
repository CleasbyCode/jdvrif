#!/bin/bash
# Focused negative-path and boundary regression tests for jdvrif.
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# ROOT is the C++ source tree. This directory may sit inside it (src/tests)
# or beside it at the repository root (tests/).
ROOT="$(cd "$TESTS/.." && pwd -P)"
[[ -f "$ROOT/compile_jdvrif.sh" ]] || ROOT="$ROOT/src"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_security_smoke.sh [options]

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
            if [[ $# -lt 2 ]]; then
                echo "--bin requires a path" >&2
                exit 2
            fi
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
need_cmd find
need_cmd "${CXX:-g++}"
need_cmd python3
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

COVER="$TESTS/testdata/covers/cover_default.jpg"
PAYLOAD="$TESTS/testdata/payloads/payload_text.txt"

if [[ ! -f "$COVER" || ! -f "$PAYLOAD" ]]; then
    bash "$TESTS/create_testdata.sh" >/dev/null
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-security.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" JPG image: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -nE 's/.*Recovery KEY: \[\*\*\*([1-9A-HJ-NP-Za-km-z-]+)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

# AddressSanitizer reserves a multi-terabyte shadow mapping at startup, so an
# instrumented binary aborts under `ulimit -v` before reaching any jdvrif code.
# Tests that use an address-space cap must drop it for such a build. Detect this
# from the binary itself: ASAN_OPTIONS is only set if the caller exported it,
# which makes it an unreliable signal when running the suite directly.
binary_is_sanitized() {
    [[ -n "${ASAN_OPTIONS:-}" ]] && return 0
    ldd "$BIN" 2>/dev/null | grep -q 'libasan' && return 0
    grep -qa '__asan_init' "$BIN" 2>/dev/null && return 0
    return 1
}

assert_no_stage_files() {
    local dir="$1"
    local found
    found="$(find "$dir" -maxdepth 1 -type f \
        \( -name '.jdvrif_*' -o -name '.*.jdvrif_tmp_*' \) -print -quit)"
    if [[ -n "$found" ]]; then
        echo "unexpected staging file remains: $found" >&2
        return 1
    fi
}

assert_no_recovered_payload() {
    local dir="$1"
    local found
    found="$(find "$dir" -maxdepth 1 \
        \( -type f -o -type l \) -name 'payload_text*' -print -quit)"
    if [[ -n "$found" ]]; then
        echo "unexpected recovered payload remains: $found" >&2
        return 1
    fi
    assert_no_stage_files "$dir"
}

PASS=0
FAIL=0
SKIP=0

# Well-formed 128-bit PINs (design test vectors) that conceal never mints in
# practice: wrong-PIN cases use these so they reach key derivation.
WRONG_PIN="1111-1111-1111-1111-1111-126X"
WRONG_PIN_ALT="112d-rXXU-ifSr-RnXL-GbXg-8Egs"
PIN128_RE='^[1-9A-HJ-NP-Za-km-z]{4}(-[1-9A-HJ-NP-Za-km-z]{4}){5}$'
# error_text drops blank lines, so the expected text has none.
# The messages a v10 image gives ($EMBEDDED); test_prompt_word_matches_image
# covers the PIN and KEY/PIN wording of older and carrier images.
PIN_FORMAT_ERROR=$'KEY Entry Error: That is not a well-formed recovery KEY.\nA jdvrif KEY is 24 letters and digits, printed in groups of four (XXXX-XXXX-XXXX-XXXX-XXXX-XXXX).\nEnter it exactly as it was printed when the file was concealed.'
PIN_CHECK_ERROR='KEY Entry Error: That KEY has a typo: its last two characters do not match the rest. Check each character, including upper and lower case.'
MISMATCH_NEEDS_PIN128='File Extraction Error: This image was made by jdvrif v10 or later and needs its 24-character recovery KEY.'
MISMATCH_NEEDS_LEGACY='File Extraction Error: This image was made before jdvrif v10 and needs the numeric recovery PIN it was created with.'

run_test() {
    local name="$1"
    shift
    local rc
    # The test body runs in a subshell that is NOT itself a condition, so its
    # errexit stays armed: a bare assertion (grep -q, cmp -s, [[ ... ]]) that
    # fails aborts the test instead of being silently ignored. `if (set -e;
    # "$@")` would put the function in a condition context, where bash disables
    # errexit for the whole call -- which made every such assertion a no-op.
    set +e
    ( set -euo pipefail; "$@" )
    rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        echo "[PASS] $name"
        PASS=$((PASS + 1))
    else
        if [[ "$rc" -eq 77 ]]; then
            echo "[SKIP] $name"
            SKIP=$((SKIP + 1))
        else
            echo "[FAIL] $name" >&2
            FAIL=$((FAIL + 1))
        fi
    fi
}

prepare_fresh_fixture() {
    local dir="$WORK/fresh"
    mkdir -p "$dir"
    (
        cd "$dir"
        "$BIN" conceal "$COVER" "$PAYLOAD" > conceal.log 2>&1
    )

    local embedded_name
    embedded_name="$(extract_embedded_image "$dir/conceal.log")"
    PIN="$(extract_pin "$dir/conceal.log")"
    if [[ -z "$embedded_name" || -z "$PIN" || ! -f "$dir/$embedded_name" ]]; then
        echo "Failed to create fresh default fixture:" >&2
        cat "$dir/conceal.log" >&2
        exit 1
    fi
    EMBEDDED="$dir/$embedded_name"
}

prepare_fresh_fixture

test_build_script_from_other_directory() {
    local dir="$WORK/build_cwd"
    mkdir -p "$dir"
    (
        cd "$dir"
        TARGET="$dir/mock-jdvrif" \
            bash "$ROOT/compile_jdvrif.sh" > build.log 2>&1
    )
    grep -q 'Compilation successful' "$dir/build.log"
    [[ -x "$dir/mock-jdvrif" ]]
}

test_info_documentation() {
    local info="$WORK/info.txt"
    "$BIN" --info > "$info"
    grep -q 'cmake' "$info"
    grep -q 'ninja-build' "$info"
    grep -q 'util-linux' "$info"
    grep -q 'zlib1g-dev' "$info"
    grep -q 'libdeflate-dev' "$info"
    grep -q 'bsky/create_bsky_post.py' "$info"
    grep -q 'ATP_AUTH_PASSWORD' "$info"
    grep -q 'alt text for image 1' "$info"
    grep -q 'alt text for image 2' "$info"
    grep -Fq 'python3 bsky/create_bsky_post.py \' "$info"
    grep -Fq 'jdvrif capsize [-r|-x] <cover_image>' "$info"
    grep -Fq 'only to conceal -r (Reddit) and conceal -x (X-Twitter)' "$info"
    grep -Fq 'leave at least 1 KiB of margin' "$info"
    grep -Fq 'dedicated steganalysis may reveal' "$info"
    if grep -q -- '--password xxxx' "$info"; then
        echo "info text still recommends exposing an app password on the command line" >&2
        return 1
    fi
}

# X-Twitter prepends a JFIF APP0 to default-mode uploads and keeps the rest
# byte-for-byte; recover must walk past it. A shift with no segment to account
# for it, or a non-APP segment in front, must still not be recognised.
test_leading_segments_before_icc() {
    local dir="$WORK/leading_segments_icc"
    local recovered case
    mkdir -p "$dir/jfif" "$dir/shifted" "$dir/dqt"
    python3 - "$EMBEDDED" "$dir" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
out = Path(sys.argv[2])
jfif = bytes.fromhex("ffe000104a46494600010100000100010000")
(out / "jfif" / "input.jpg").write_bytes(data[:2] + jfif + data[2:])
(out / "shifted" / "input.jpg").write_bytes(data[:2] + b"\0" * 18 + data[2:])
(out / "dqt" / "input.jpg").write_bytes(data[:2] + b"\xff\xdb\x00\x04AB" + data[2:])
PY
    (cd "$dir/jfif" && printf '%s\n' "$PIN" | "$BIN" recover input.jpg > recover.log 2>&1)
    recovered="$(extract_recovered_file "$dir/jfif/recover.log")"
    [[ -n "$recovered" ]] || { cat "$dir/jfif/recover.log" >&2; return 1; }
    cmp -s "$dir/jfif/$recovered" "$PAYLOAD"

    for case in shifted dqt; do
        if (cd "$dir/$case" && printf '%s\n' "$PIN" | "$BIN" recover input.jpg > recover.log 2>&1); then
            echo "recover accepted an unsupported ICC layout ($case)" >&2
            return 1
        fi
        if [[ "$case" == shifted ]]; then
            grep -Fq 'Invalid KEY/PIN, or this is not a valid jdvrif "file-embedded" image' "$dir/$case/recover.log"
        fi
        assert_no_recovered_payload "$dir/$case"
    done
}

test_wrong_pin() {
    local dir="$WORK/wrong_pin"
    local wrong_pin="$WRONG_PIN"
    if [[ "$PIN" == "$wrong_pin" ]]; then wrong_pin="$WRONG_PIN_ALT"; fi
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    if (
        cd "$dir"
        printf '%s\n' "$wrong_pin" | "$BIN" recover input.jpg > recover.log 2>&1
    ); then
        echo "recovery unexpectedly accepted a wrong PIN" >&2
        return 1
    fi
    assert_no_recovered_payload "$dir"
}

# A wrong PIN must be rejected from the first frame, read where the
# ciphertext sits, without copying it anywhere first. RLIMIT_FSIZE makes that
# filesystem-independent: recovery under an 8 MiB cap can only report the bad
# PIN if it never writes the 32 MiB ciphertext out.
test_wrong_pin_stages_no_ciphertext() {
    local dir="$WORK/wrong_pin_no_staging"
    mkdir -p "$dir"
    cp "$COVER" "$dir/cover.jpg"
    head -c $((32 * 1024 * 1024)) /dev/urandom > "$dir/payload.bin"
    (
        cd "$dir"
        "$BIN" conceal cover.jpg payload.bin > conceal.log 2>&1
    )
    local image pin wrong_pin="$WRONG_PIN"
    image="$(extract_embedded_image "$dir/conceal.log")"
    pin="$(extract_pin "$dir/conceal.log")"
    [[ -n "$image" && -n "$pin" ]] || { echo "conceal of the large payload failed" >&2; return 1; }
    if [[ "$pin" == "$wrong_pin" ]]; then wrong_pin="$WRONG_PIN_ALT"; fi
    rm -f "$dir/payload.bin"
    if (
        cd "$dir"
        ulimit -f 8192
        printf '%s\n' "$wrong_pin" | "$BIN" recover "$image" > recover.log 2>&1
    ); then
        echo "recovery unexpectedly accepted a wrong PIN" >&2
        return 1
    fi
    if ! grep -Fq 'Invalid recovery KEY or file is corrupt.' "$dir/recover.log"; then
        echo "wrong PIN was not rejected before staging I/O:" >&2
        cat "$dir/recover.log" >&2
        return 1
    fi
    [[ ! -e "$dir/payload.bin" ]] || { echo "wrong PIN recovered a payload" >&2; return 1; }
    assert_no_recovered_payload "$dir"
}

test_pin_entry_rejects_non_digits() {
    local dir="$WORK/pin_non_digit"
    local entered
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    # A stray or mistyped keystroke must reject the PIN, not be dropped from it.
    for entered in "${PIN:0:3}x${PIN:3}" "${PIN:0:5}O${PIN:5}" "***${PIN}***"; do
        if (
            cd "$dir"
            printf '%s\n' "$entered" | "$BIN" recover input.jpg > recover.log 2>&1
        ); then
            echo "recovery accepted a PIN containing a stray keystroke: $entered" >&2
            return 1
        fi
        if ! grep -Fq 'not a well-formed recovery KEY.' "$dir/recover.log"; then
            echo "PIN with a stray keystroke was not reported as malformed: $entered" >&2
            return 1
        fi
        assert_no_recovered_payload "$dir"
    done
    # Whitespace is ignored, and a backspaced stray keystroke no longer counts.
    (
        cd "$dir"
        printf ' %s %s\tx\b\n' "${PIN:0:4}" "${PIN:4}" | "$BIN" recover input.jpg > recover.log 2>&1
    )
    grep -q 'Complete! Please check your file.' "$dir/recover.log"
}

test_authenticated_compression_mode() {
    local dir="$WORK/compression_tamper"
    mkdir -p "$dir"
    python3 - "$EMBEDDED" "$dir/input.jpg" "$dir/downgrade.jpg" <<'PY'
import sys
from pathlib import Path

data = bytearray(Path(sys.argv[1]).read_bytes())
mntr = data.find(b"mntrRGB")
if mntr < 8:
    raise SystemExit("ICC signature not found")
base = mntr - 8
flag = base + 0x68
if flag >= len(data):
    raise SystemExit("compression marker out of bounds")
if data[flag] == 0x58:
    raise SystemExit("fresh small payload unexpectedly bypassed compression")
data[flag] = 0x58
Path(sys.argv[2]).write_bytes(data)

# The version marker is mutable metadata too. Downgrading an authenticated
# framing to legacy KDF2 must not remove the associated-data check and
# resurrect the old ambiguity. Accept any framing that authenticates the mode
# (KDF3, KDF4, KDF5, ...) so retiring a mint version stays a one-line change here.
kdf = base + 0x2FB
authenticated = (b"KDF3", b"KDF4", b"KDF5")
if bytes(data[kdf:kdf + 4]) not in authenticated:
    raise SystemExit(
        f"fresh fixture did not use an authenticated KDF framing: {bytes(data[kdf:kdf+4])!r}")
data[kdf:kdf + 4] = b"KDF2"
Path(sys.argv[3]).write_bytes(data)
PY
    for image in input.jpg downgrade.jpg; do
        if (
            cd "$dir"
            printf '%s\n' "$PIN" | "$BIN" recover "$image" > "recover-$image.log" 2>&1
        ); then
            echo "recovery accepted tampered compression semantics from $image" >&2
            return 1
        fi
    done
    assert_no_recovered_payload "$dir"
}

test_legacy_recovery_warning() {
    local dir="$WORK/legacy_warning"
    mkdir -p "$dir"
    python3 - "$BIN" "$TESTS" "$dir" "$EMBEDDED" "$PIN" <<'PY'
import csv
import subprocess
import sys
import zlib
from pathlib import Path

binary, tests, work, current_image, current_pin = sys.argv[1:]
tests, work = Path(tests), Path(work)
with (tests / "golden/manifest.tsv").open() as manifest:
    case = next(row for row in csv.DictReader(manifest, delimiter="\t")
                if row["case_id"] == "default_single")
legacy = bytearray((tests / case["golden_rel"]).read_bytes())
base = legacy.index(b"mntrRGB") - 8
assert legacy[base + 0x2FB:base + 0x2FF] == b"KDF2"
expected = (tests / case["payload_rel"]).read_bytes()
warning = "Warning: Legacy KDF2 image: the compression flag is unauthenticated"

for name in ("legacy", "altered", "current"):
    directory = work / name
    directory.mkdir()
    data = bytearray(legacy)
    pin = case["pin"]
    if name == "altered":
        data[base + 0x68] = 0x58
    elif name == "current":
        data = Path(current_image).read_bytes()
        pin = current_pin
    (directory / "input.jpg").write_bytes(data)
    result = subprocess.run([binary, "recover", "input.jpg"], cwd=directory,
                            input=pin + "\n", text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stderr.count(warning) == (0 if name == "current" else 1), result.stderr
    if name != "current":
        recovered = (directory / Path(case["payload_rel"]).name).read_bytes()
        if name == "altered":
            assert recovered != expected
            recovered = zlib.decompress(recovered)
        assert recovered == expected
PY
}

# KDF4/KDF5 record the Argon2id cost parameters in the image. Out-of-range costs
# drive an allocation and a work loop that both run before a wrong PIN can be
# detected, so they must be refused outright -- and refused without asking the
# user for a PIN first, like every other unusable-metadata rejection.
test_kdf_cost_bounds() {
    local dir="$WORK/kdf_costs"
    mkdir -p "$dir"
    python3 - "$EMBEDDED" "$dir" <<'KDFPY'
import sys
from pathlib import Path

data = bytearray(Path(sys.argv[1]).read_bytes())
out = Path(sys.argv[2])
mntr = data.find(b"mntrRGB")
if mntr < 8:
    raise SystemExit("ICC signature not found")
base = mntr - 8
kdf = base + 0x2FB
if bytes(data[kdf:kdf + 4]) != b"KDF5":
    raise SystemExit(f"fixture is not KDF5: {bytes(data[kdf:kdf + 4])!r}")

ops = kdf + 48
mem = kdf + 52
if int.from_bytes(data[ops:ops + 4], "big") != 2:
    raise SystemExit("unexpected recorded opslimit")
if int.from_bytes(data[mem:mem + 4], "big") != 64 * 1024 * 1024:
    raise SystemExit("unexpected recorded memlimit")

cases = {
    "huge_mem.jpg": (mem, 0xFFFFFFFF),
    "zero_mem.jpg": (mem, 0),
    "huge_ops.jpg": (ops, 0xFFFFFFFF),
    "zero_ops.jpg": (ops, 0),
}
for name, (offset, value) in cases.items():
    patched = bytearray(data)
    patched[offset:offset + 4] = value.to_bytes(4, "big")
    (out / name).write_bytes(patched)
KDFPY

    local image
    for image in huge_mem.jpg zero_mem.jpg huge_ops.jpg zero_ops.jpg; do
        if (
            cd "$dir"
            printf '%s\n' "$PIN" | "$BIN" recover "$image" > "recover-$image.log" 2>&1
        ); then
            echo "recovery accepted out-of-range KDF costs from $image" >&2
            return 1
        fi
        if ! grep -Fq 'unsupported key-derivation parameters' "$dir/recover-$image.log"; then
            echo "wrong rejection reason for $image:" >&2
            cat "$dir/recover-$image.log" >&2
            return 1
        fi
        if grep -Eq '(KEY|PIN): ' "$dir/recover-$image.log"; then
            echo "PIN was requested before rejecting bad KDF costs in $image" >&2
            return 1
        fi
    done
    assert_no_recovered_payload "$dir"
}

test_truncated_ciphertext() {
    local dir="$WORK/truncated"
    mkdir -p "$dir"
    python3 - "$EMBEDDED" "$dir/input.jpg" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
mntr = data.find(b"mntrRGB")
if mntr < 8:
    raise SystemExit("ICC signature not found")
base = mntr - 8
size = int.from_bytes(data[base + 0x2CA:base + 0x2CE], "big")
payload = base + 0x33B
if size < 2 or payload + size > len(data):
    raise SystemExit("unexpected embedded layout")
Path(sys.argv[2]).write_bytes(data[:payload + size - 1])
PY
    if (
        cd "$dir"
        printf '%s\n' "$PIN" | "$BIN" recover input.jpg > recover.log 2>&1
    ); then
        echo "recovery accepted truncated ciphertext" >&2
        return 1
    fi
    assert_no_recovered_payload "$dir"
}

patch_declared_size() {
    local input="$1"
    local output="$2"
    local value="$3"
    python3 - "$input" "$output" "$value" <<'PY'
import sys
from pathlib import Path

data = bytearray(Path(sys.argv[1]).read_bytes())
value = int(sys.argv[3])
mntr = data.find(b"mntrRGB")
if mntr < 8:
    raise SystemExit("ICC signature not found")
base = mntr - 8
data[base + 0x2CA:base + 0x2CE] = value.to_bytes(4, "big")
Path(sys.argv[2]).write_bytes(data)
PY
}

assert_size_boundary_behavior() {
    local image="$1"
    local pin="$2"
    local max_size="$3"
    local tag="$4"
    local dir="$WORK/$tag"
    mkdir -p "$dir/accepted" "$dir/rejected"

    patch_declared_size "$image" "$dir/accepted/input.jpg" "$max_size"
    if (
        cd "$dir/accepted"
        printf '%s\n' "$pin" | "$BIN" recover input.jpg > recover.log 2>&1
    ); then
        echo "synthetic oversized fixture unexpectedly recovered" >&2
        return 1
    fi
    if ! grep -Eq '(KEY|PIN): ' "$dir/accepted/recover.log"; then
        echo "recovery rejected the documented wiggle-room boundary before PIN entry" >&2
        cat "$dir/accepted/recover.log" >&2
        return 1
    fi
    assert_no_recovered_payload "$dir/accepted"

    patch_declared_size "$image" "$dir/rejected/input.jpg" "$((max_size + 1))"
    if (
        cd "$dir/rejected"
        printf '%s\n' "$pin" | "$BIN" recover input.jpg > recover.log 2>&1
    ); then
        echo "recovery accepted a declaration beyond its wiggle-room cap" >&2
        return 1
    fi
    if grep -Eq '(KEY|PIN): ' "$dir/rejected/recover.log"; then
        echo "recovery prompted for a PIN before rejecting an over-cap declaration" >&2
        return 1
    fi
    grep -q 'exceeds the maximum allowed' "$dir/rejected/recover.log"
    assert_no_recovered_payload "$dir/rejected"
}

test_default_recovery_wiggle_room() {
    assert_size_boundary_behavior \
        "$EMBEDDED" "$PIN" "$((2 * 1024 * 1024 * 1024 + 50 * 1024 * 1024))" \
        default_size_boundary
}

test_dangling_symlink_collision() {
    local dir="$WORK/dangling_symlink"
    local recovered
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    ln -s missing-target "$dir/payload_text.txt"
    (
        cd "$dir"
        printf '%s\n' "$PIN" | "$BIN" recover input.jpg > recover.log 2>&1
    )
    recovered="$(extract_recovered_file "$dir/recover.log")"
    if [[ "$recovered" != "payload_text_1.txt" ]]; then
        echo "expected payload_text_1.txt, got ${recovered:-<none>}" >&2
        return 1
    fi
    [[ -L "$dir/payload_text.txt" ]]
    [[ "$(readlink "$dir/payload_text.txt")" == "missing-target" ]]
    cmp -s "$dir/$recovered" "$PAYLOAD"
    assert_no_stage_files "$dir"
}

test_pin_delivery_failure_is_transactional() {
    local dir="$WORK/full_stdout"
    local found
    mkdir -p "$dir"
    if (
        cd "$dir"
        "$BIN" conceal "$COVER" "$PAYLOAD" > /dev/full 2> conceal.err
    ); then
        echo "conceal reported success even though the PIN could not be delivered" >&2
        return 1
    fi
    found="$(find "$dir" -maxdepth 1 \
        \( -name 'jrif_*.jpg' -o -name '.jdvrif_*' -o -name '.*.jdvrif_tmp_*' \) \
        -print -quit)"
    if [[ -n "$found" ]]; then
        echo "conceal committed or leaked an output after PIN delivery failed: $found" >&2
        return 1
    fi
}

test_bluesky_final_output_size_limit() {
    local dir="$WORK/bluesky_final_size"
    local accepted_payload="$TESTS/testdata/payloads/bsingle.bin"
    local rejected_payload="$TESTS/testdata/payloads/bxmp.bin"
    local embedded accepted_size found

    if ! command -v ffmpeg >/dev/null 2>&1; then
        echo "ffmpeg is unavailable; near-limit Bluesky fixture not generated" >&2
        return 77
    fi
    if [[ ! -f "$accepted_payload" || ! -f "$rejected_payload" ]]; then
        bash "$TESTS/create_testdata.sh" >/dev/null
    fi

    mkdir -p "$dir/accepted" "$dir/rejected"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi \
        -i 'nullsrc=size=1770x1770,noise=alls=100:allf=u:all_seed=12345' \
        -frames:v 1 -q:v 2 "$dir/cover.jpg"

    (
        cd "$dir/accepted"
        "$BIN" conceal -b "$dir/cover.jpg" "$accepted_payload" > conceal.log 2>&1
    )
    embedded="$(extract_embedded_image "$dir/accepted/conceal.log")"
    if [[ -z "$embedded" || ! -f "$dir/accepted/$embedded" ]]; then
        echo "near-limit Bluesky control did not produce an output image" >&2
        cat "$dir/accepted/conceal.log" >&2
        return 1
    fi
    accepted_size="$(stat -c '%s' "$dir/accepted/$embedded")"
    if [[ "$accepted_size" -gt 2000000 ]]; then
        echo "Bluesky conceal accepted a $accepted_size-byte output image" >&2
        return 1
    fi
    assert_no_stage_files "$dir/accepted"

    if (
        cd "$dir/rejected"
        "$BIN" conceal -b "$dir/cover.jpg" "$rejected_payload" > conceal.log 2>&1
    ); then
        echo "Bluesky conceal accepted a final output image over 2,000,000 bytes" >&2
        return 1
    fi
    grep -Fq \
        'File Size Error: Final output image exceeds the 2,000,000-byte limit for the Bluesky platform.' \
        "$dir/rejected/conceal.log"
    grep -Fq \
        '                 Use a smaller cover image or reduce the size of the payload (hidden data file).' \
        "$dir/rejected/conceal.log"
    if grep -Fq 'Recovery KEY:' "$dir/rejected/conceal.log"; then
        echo "Bluesky conceal disclosed a PIN for a rejected output image" >&2
        return 1
    fi
    found="$(find "$dir/rejected" -maxdepth 1 -type f -name 'jrif_*.jpg' -print -quit)"
    if [[ -n "$found" ]]; then
        echo "Bluesky conceal committed an oversized output image: $found" >&2
        return 1
    fi
    assert_no_stage_files "$dir/rejected"
}

test_oversized_jpeg_dimensions() {
    local dir="$WORK/huge_dimensions"
    local rc
    mkdir -p "$dir"
    python3 - "$COVER" "$dir/huge.jpg" <<'PY'
import sys
from pathlib import Path

data = bytearray(Path(sys.argv[1]).read_bytes())
pos = 2
sof = None
while pos + 4 <= len(data):
    if data[pos] != 0xFF:
        pos += 1
        continue
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    marker = data[pos]
    pos += 1
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    if marker == 0xDA:
        break
    length = int.from_bytes(data[pos:pos + 2], "big")
    if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                  0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
        sof = pos + 2
        break
    pos += length
if sof is None:
    raise SystemExit("SOF marker not found")
data[sof + 1:sof + 3] = (65000).to_bytes(2, "big")
data[sof + 3:sof + 5] = (65000).to_bytes(2, "big")
Path(sys.argv[2]).write_bytes(data)
PY
    local capped=1
    if binary_is_sanitized; then
        capped=0
    fi

    if command -v timeout >/dev/null 2>&1; then
        set +e
        (
            cd "$dir"
            if [[ "$capped" -eq 1 ]]; then ulimit -v 524288; fi
            timeout 10 "$BIN" conceal huge.jpg "$PAYLOAD" > conceal.log 2>&1
        )
        rc=$?
        set -e
        if [[ "$rc" -eq 0 || "$rc" -eq 124 || "$rc" -eq 137 ]]; then
            echo "oversized-dimension rejection returned unsafe status $rc" >&2
            return 1
        fi
    else
        if (
            cd "$dir"
            if [[ "$capped" -eq 1 ]]; then ulimit -v 524288; fi
            "$BIN" conceal huge.jpg "$PAYLOAD" > conceal.log 2>&1
        ); then
            echo "oversized JPEG dimensions were accepted" >&2
            return 1
        fi
    fi
    grep -Eiq 'dimension|pixel|too large|maximum' "$dir/conceal.log"
}

test_default_dimension_limit() {
    local dir="$WORK/default_dimension_limit"
    mkdir -p "$dir"
    python3 - "$COVER" "$dir/over_width.jpg" <<'PY'
import sys
from pathlib import Path

data = bytearray(Path(sys.argv[1]).read_bytes())
pos = 2
sof = None
while pos + 4 <= len(data):
    if data[pos] != 0xFF:
        pos += 1
        continue
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    marker = data[pos]
    pos += 1
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    if marker == 0xDA:
        break
    length = int.from_bytes(data[pos:pos + 2], "big")
    if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                  0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
        sof = pos + 2
        break
    pos += length
if sof is None:
    raise SystemExit("SOF marker not found")
data[sof + 3:sof + 5] = (4097).to_bytes(2, "big")
Path(sys.argv[2]).write_bytes(data)
PY

    if (
        cd "$dir"
        "$BIN" conceal over_width.jpg "$PAYLOAD" > conceal.log 2>&1
    ); then
        echo "4097-pixel-wide default cover was unexpectedly accepted" >&2
        return 1
    fi
    grep -Fq 'exceed the safe cover-image limit (4096px per dimension and 16777216 total pixels)' "$dir/conceal.log"
    assert_no_stage_files "$dir"
}

test_cmyk_cover_rejected() {
    local dir="$WORK/cmyk"
    if ! python3 -c 'import PIL' >/dev/null 2>&1; then
        echo "Pillow is unavailable; CMYK fixture not generated" >&2
        return 77
    fi
    mkdir -p "$dir"
    python3 - "$dir/cmyk.jpg" <<'PY'
import sys
from PIL import Image

Image.new("CMYK", (500, 500), (0, 255, 255, 0)).save(
    sys.argv[1], format="JPEG", quality=90
)
PY
    if (
        cd "$dir"
        "$BIN" conceal cmyk.jpg "$PAYLOAD" > conceal.log 2>&1
    ); then
        echo "CMYK cover was accepted" >&2
        return 1
    fi
    grep -Eiq 'CMYK|YCCK|color space|unsupported' "$dir/conceal.log"
}

test_post_transform_dimensions() {
    local dir="$WORK/trimmed_dimensions"
    local magick_bin
    magick_bin="$(command -v magick || command -v convert || true)"
    if [[ -z "$magick_bin" ]]; then
        echo "ImageMagick is unavailable; 4:1:1 fixture not generated" >&2
        return 77
    fi
    mkdir -p "$dir"
    "$magick_bin" -size 400x401 gradient:red-blue -colorspace sRGB \
        -sampling-factor 4:1:1 -quality 90 "$dir/base.jpg"
    python3 - "$dir/base.jpg" "$dir/oriented.jpg" <<'PY'
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if not data.startswith(b"\xff\xd8"):
    raise SystemExit("not a JPEG")
tiff = (
    b"II\x2a\x00" + struct.pack("<I", 8) + struct.pack("<H", 1) +
    struct.pack("<HHI", 0x0112, 3, 1) + struct.pack("<H", 2) + b"\x00\x00" +
    struct.pack("<I", 0)
)
payload = b"Exif\x00\x00" + tiff
app1 = b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload
Path(sys.argv[2]).write_bytes(data[:2] + app1 + data[2:])
PY
    if (
        cd "$dir"
        "$BIN" conceal oriented.jpg "$PAYLOAD" > conceal.log 2>&1
    ); then
        echo "cover trimmed below 400px was accepted" >&2
        return 1
    fi
    grep -Eiq 'dimension|too small|at least 400' "$dir/conceal.log"
}

test_referenced_nonzero_dqt_quality() {
    local dir="$WORK/nonzero_dqt"
    local mode option embedded
    if ! python3 -c 'import PIL' >/dev/null 2>&1; then
        echo "Pillow is unavailable; DQT fixture not generated" >&2
        return 77
    fi
    mkdir -p "$dir"
    python3 - "$dir/dqt1.jpg" <<'PY'
import io
import sys
from pathlib import Path
from PIL import Image

buf = io.BytesIO()
Image.new("L", (500, 500), 128).save(buf, format="JPEG", quality=100)
data = bytearray(buf.getvalue())
pos = 2
changed_dqt = False
changed_sof = False
while pos + 4 <= len(data):
    if data[pos] != 0xFF:
        pos += 1
        continue
    marker_start = pos
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    marker = data[pos]
    pos += 1
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    if marker == 0xDA:
        break
    length = int.from_bytes(data[pos:pos + 2], "big")
    payload = pos + 2
    end = pos + length
    if marker == 0xDB:
        cursor = payload
        while cursor < end:
            header = data[cursor]
            precision = header >> 4
            table_size = 128 if precision else 64
            if (header & 0x0F) == 0:
                data[cursor] = (header & 0xF0) | 1
                changed_dqt = True
            cursor += 1 + table_size
    elif marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                    0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
        components = data[payload + 5]
        for index in range(components):
            qsel = payload + 6 + index * 3 + 2
            if data[qsel] == 0:
                data[qsel] = 1
                changed_sof = True
    pos = end
if not (changed_dqt and changed_sof):
    raise SystemExit("failed to build nonzero-DQT fixture")
Path(sys.argv[1]).write_bytes(data)
PY
    if (
        cd "$dir"
        "$BIN" conceal dqt1.jpg "$PAYLOAD" > conceal.log 2>&1
    ); then
        echo "quality-100 cover using DQT table 1 was accepted" >&2
        return 1
    fi
    grep -Eiq 'quality.*exceeds|quality.*maximum' "$dir/conceal.log"
    assert_no_stage_files "$dir"

    mkdir -p "$dir/bluesky"
    if ! (
        cd "$dir/bluesky"
        "$BIN" conceal -b "$dir/dqt1.jpg" "$PAYLOAD" > conceal.log 2>&1
    ); then
        echo "quality-100 cover was rejected in bluesky mode" >&2
        cat "$dir/bluesky/conceal.log" >&2
        return 1
    fi
    embedded="$(extract_embedded_image "$dir/bluesky/conceal.log")"
    if [[ -z "$embedded" || ! -f "$dir/bluesky/$embedded" ]]; then
        echo "bluesky mode did not produce an output for the quality-100 cover" >&2
        cat "$dir/bluesky/conceal.log" >&2
        return 1
    fi
    assert_no_stage_files "$dir/bluesky"
}

test_terminal_restored_after_interrupt() {
    local dir="$WORK/terminal_interrupt"
    mkdir -p "$dir"
    python3 - "$BIN" "$EMBEDDED" "$dir" <<'PY'
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time

binary, image, work = sys.argv[1:]
master, slave = pty.openpty()
before = termios.tcgetattr(slave)
proc = subprocess.Popen(
    [binary, "recover", image],
    cwd=work,
    stdin=slave,
    stdout=slave,
    stderr=slave,
    close_fds=True,
    start_new_session=True,
)
output = bytearray()
deadline = time.monotonic() + 10
try:
    while not (b"KEY: " in output or b"PIN: " in output) and time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                output.extend(os.read(master, 4096))
            except OSError:
                break
        if proc.poll() is not None:
            break
    if not (b"KEY: " in output or b"PIN: " in output):
        raise RuntimeError(f"PIN prompt not observed: {bytes(output)!r}")

    during = termios.tcgetattr(slave)
    mask = termios.ECHO | termios.ICANON
    if during[3] & mask:
        raise RuntimeError("PIN input did not disable echo and canonical input")

    os.killpg(proc.pid, signal.SIGINT)
    try:
        status = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        raise RuntimeError("process did not exit after SIGINT")
    if status == 0:
        raise RuntimeError("interrupted recovery returned success")

    after = termios.tcgetattr(slave)
    if (after[3] & mask) != (before[3] & mask):
        raise RuntimeError("terminal echo/canonical flags were not restored")
finally:
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
    os.close(master)
    os.close(slave)
PY
    assert_no_recovered_payload "$dir"
}

# The compression stage holds the deflated payload, which is plaintext-
# equivalent. It must live on a link-free inode (StagingFile): never present as
# a directory entry, so it cannot be listed, backed up or sync-uploaded, and
# cannot outlive a SIGKILL. Detect it through /proc/<pid>/fd, where such an
# inode appears as "<dir>/... (deleted)", and assert it never appears by name.
staging_inode_open() {
    local pid="$1" dir="$2" fd target
    for fd in /proc/"$pid"/fd/*; do
        target="$(readlink "$fd" 2>/dev/null)" || continue
        case "$target" in
            "$dir"/*" (deleted)") return 0 ;;
        esac
    done
    return 1
}

test_signal_cleans_compression_stage() {
    local dir="$WORK/signal_cleanup"
    local pid status seen=0
    if ! command -v truncate >/dev/null 2>&1; then
        echo "truncate is unavailable; sparse interrupt fixture not generated" >&2
        return 77
    fi
    mkdir -p "$dir"
    truncate -s $((512 * 1024 * 1024)) "$dir/source.bin"
    (
        cd "$dir"
        exec "$BIN" conceal "$COVER" source.bin > conceal.log 2>&1
    ) &
    pid=$!

    for ((attempt = 0; attempt < 1000; ++attempt)); do
        # A named staging file at any point is the failure this test exists for.
        if find "$dir" -maxdepth 1 -type f \
            \( -name '.jdvrif_*' -o -name '.*.jdvrif_tmp_*' \) -print -quit | grep -q .; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            echo "compression stage appeared as a named file in the working directory" >&2
            return 1
        fi
        if staging_inode_open "$pid" "$dir"; then
            seen=1
            kill -TERM "$pid"
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        sleep 0.01
    done

    if [[ "$seen" -eq 0 ]]; then
        if kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid"; fi
        wait "$pid" 2>/dev/null || true
        echo "did not observe the anonymous compression staging inode before process exit" >&2
        return 1
    fi

    set +e
    wait "$pid"
    status=$?
    set -e
    if [[ "$status" -eq 0 ]]; then
        echo "signal-interrupted conceal returned success" >&2
        return 1
    fi
    assert_no_stage_files "$dir"
    if find "$dir" -maxdepth 1 -type f -name 'jrif_*.jpg' -print -quit | grep -q .; then
        echo "signal-interrupted conceal committed an output image" >&2
        return 1
    fi
}

test_wipe_guards_unwind() {
    local dir="$WORK/wipe_guards"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -I"$ROOT" "$TESTS/test_wipe_guards.cpp" -lsodium -o "$dir/test_wipe_guards"
    "$dir/test_wipe_guards"
}

test_inflater_wipes_plaintext() {
    local dir="$WORK/inflate_wipe"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O1 -fsized-deallocation -I"$ROOT" -I"$TESTS/.." \
        "$TESTS/test_inflate_wipe.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
        -Wl,--wrap=_ZdlPvm,--wrap=malloc,--wrap=free -lsodium -lz -o "$dir/test_inflate_wipe"
    (cd "$dir" && ./test_inflate_wipe)
}

test_pin_codec_vectors() {
    local dir="$WORK/pin_codec"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -Wall -Wextra -I"$ROOT" \
        "$TESTS/test_pin_codec.cpp" "$ROOT/pin_codec.cpp" -lsodium -o "$dir/test_pin_codec"
    "$dir/test_pin_codec"
}

test_carrier_prf() {
    local dir="$WORK/carrier_prf"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -Wall -Wextra -Wconversion -I"$ROOT" \
        "$TESTS/test_carrier_prf.cpp" -lsodium -o "$dir/test_carrier_prf"
    "$dir/test_carrier_prf"
}

test_carrier_key128() {
    local dir="$WORK/carrier_key128"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -I"$ROOT" \
        "$TESTS/test_carrier_key128.cpp" \
        "$ROOT/encryption_kdf.cpp" \
        "$ROOT/binary_io.cpp" \
        "$ROOT/file_utils.cpp" \
        "$ROOT/signal_utils.cpp" \
        -lsodium -o "$dir/test_carrier_key128"
    "$dir/test_carrier_key128"
}

test_reddit_carrier() {
    local dir="$WORK/reddit_carrier"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O2 -DNDEBUG \
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
        -I"$ROOT" \
        "$TESTS/test_reddit_carrier.cpp" \
        "$ROOT/reddit_steg.cpp" \
        "$ROOT/signal_utils.cpp" \
        "$ROOT/encryption_kdf.cpp" \
        "$ROOT/binary_io.cpp" \
        "$ROOT/file_utils.cpp" \
        -ljpeg -lsodium -o "$dir/test_reddit_carrier"
    "$dir/test_reddit_carrier" "$COVER" "$PAYLOAD" \
        "$TESTS/testdata/legacy_v9_1/reddit.jpg" "$TESTS/testdata/legacy_v9_1/reddit.pin"
}

test_base64_wrapper_strictness() {
    local dir="$WORK/base64"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -Wall -Wextra -I"$ROOT" \
        "$TESTS/test_base64.cpp" "$ROOT/base64.cpp" "$ROOT/binary_io.cpp" \
        "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" -lsodium -o "$dir/test_base64"
    "$dir/test_base64"
}

test_deflater_wipes_plaintext() {
    local dir="$WORK/deflate_wipe"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O1 -I"$ROOT" -I"$TESTS/.." \
        "$TESTS/test_deflate_wipe.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
        -Wl,--wrap=malloc,--wrap=free -lsodium -lz -ldeflate -o "$dir/test_deflate_wipe"
    (cd "$dir" && ./test_deflate_wipe)
}

test_input_readers_leave_no_plaintext() {
    local dir="$WORK/input_wipe"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O1 -I"$ROOT" \
        "$TESTS/test_plaintext_input_wipe.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
        -lsodium -lz -ldeflate -o "$dir/test_plaintext_input_wipe"
    (cd "$dir" && ./test_plaintext_input_wipe)
}

test_copy_fallback_wipes_plaintext() {
    local dir="$WORK/copy_fallback_wipe"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O1 -fsized-deallocation -I"$ROOT" \
        "$TESTS/test_copy_fallback_wipe.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
        -Wl,--wrap=_ZdlPvm,--wrap=sendfile -lsodium -o "$dir/test_copy_fallback_wipe"
    (cd "$dir" && ./test_copy_fallback_wipe)
}

test_publish_fallback_is_atomic() {
    local dir="$WORK/publish_fallback"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O1 -I"$ROOT" \
        "$TESTS/test_publish_fallback.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
        -Wl,--wrap=linkat,--wrap=renameat2,--wrap=link -lsodium -o "$dir/test_publish_fallback"
    (cd "$dir" && ./test_publish_fallback)
}

test_jpeg_safety() {
    bash "$TESTS/run_jpeg_safety_tests.sh"
}

test_io_security() {
    bash "$TESTS/run_io_security_tests.sh" --bin "$BIN"
}

# After the recovery PIN has been printed, the fsynced temp image is the only
# durable copy that PIN can open. A later commit failure (here: colliding the
# chosen output name) must leave that image in place and name it in the error.
test_image_committed_before_pin() {
    local dir="$WORK/commit_before_pin"
    mkdir -p "$dir"
    python3 - "$BIN"$'\x1f'conceal "$COVER" "$PAYLOAD" "$dir" <<'PY'
import ctypes
import fcntl
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

*command, cover, payload, work = sys.argv[1:]
command = command[0].split("\x1f")
if os.uname().machine != "x86_64":
    sys.exit(77)  # the /proc/PID/syscall check below uses x86-64 numbering
work = Path(work)

libc = ctypes.CDLL("libc.so.6", use_errno=True)
IN_CREATE = 0x00000100
IN_MOVED_TO = 0x00000080
EVENT = struct.Struct("iIII")

fd = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
if fd < 0:
    raise OSError(ctypes.get_errno(), "inotify_init1")
if libc.inotify_add_watch(fd, os.fsencode(work), IN_CREATE | IN_MOVED_TO) < 0:
    os.close(fd)
    raise OSError(ctypes.get_errno(), "inotify_add_watch")

def drain_names():
    names = []
    while True:
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            return names
        pos = 0
        while pos + EVENT.size <= len(chunk):
            _, _, _, length = EVENT.unpack_from(chunk, pos)
            pos += EVENT.size
            names.append(chunk[pos:pos + length].split(b"\x00", 1)[0].decode())
            pos += length

# Pre-fill a minimal stdout pipe so the child blocks on its first real write
# to stdout. Redirected stdout is fully buffered and nothing is flushed before
# the PIN, so that write is the PIN delivery: while it is blocked there, the
# image must already be complete under its final name.
F_SETPIPE_SZ = 1031
read_end, write_end = os.pipe()
fcntl.fcntl(write_end, F_SETPIPE_SZ, 4096)
os.set_blocking(write_end, False)
filler = 0
try:
    while True:
        filler += os.write(write_end, b"\0" * 4096)
except BlockingIOError:
    pass
os.set_blocking(write_end, True)

proc = subprocess.Popen(command + [cover, payload], cwd=work,
                        stdout=write_end, stderr=subprocess.PIPE)
os.close(write_end)
committed_at_pin = None
stdout = b""
try:
    deadline = time.monotonic() + 120
    while committed_at_pin is None:
        if proc.poll() is not None:
            raise RuntimeError("conceal exited before writing to stdout")
        if time.monotonic() > deadline:
            raise RuntimeError("conceal never blocked on its stdout write")
        try:
            call = Path(f"/proc/{proc.pid}/syscall").read_text().split()
        except OSError:
            call = []
        # Blocked in write(2) (syscall 1 on x86-64) on fd 1.
        if len(call) > 1 and call[0] == "1" and int(call[1], 16) == 1:
            committed_at_pin = [p for p in work.glob("jrif_*.jpg") if p.stat().st_size > 0]
        else:
            time.sleep(0.01)
    while True:
        data = os.read(read_end, 65536)
        if not data:
            break
        stdout += data
    _, stderr = proc.communicate(timeout=60)
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(read_end)
stdout = stdout[filler:]
created = drain_names()
os.close(fd)

log = (stdout + b"\n" + stderr).decode(errors="replace")
if proc.returncode != 0:
    raise RuntimeError(f"conceal failed:\n{log}")
if not committed_at_pin or len(committed_at_pin) != 1:
    raise RuntimeError(f"image was not committed before the PIN was shown: {committed_at_pin}\n{log}")
saved = re.search(r'Saved "file-embedded" JPG image: (\S+) ', log)
if saved is None or saved.group(1) != committed_at_pin[0].name:
    raise RuntimeError(f"reported output does not match the committed image:\n{log}")
# The only directory entries ever created: the final image, and (on a filesystem
# without O_TMPFILE) the momentary, still-empty name of a staging inode.
for name in created:
    if name == committed_at_pin[0].name or re.fullmatch(r"\.jdvrif_\w+_[0-9a-f]+\.tmp", name):
        continue
    raise RuntimeError(f"conceal exposed an unexpected directory entry: {name}")
PY
}

# --- 128-bit recovery PINs (jdvrif v10) -----------------------------------

# Stderr of a failed run, without the blank lines main() frames it with.
error_text() {
    sed '/^$/d' "$1"
}

# roundtrip_pin128 <tag> <conceal option or "."> <cover> <payload>
# Conceal, check the printed PIN is grouped Base58, recover with it, cmp.
roundtrip_pin128() {
    local tag="$1" option="$2" cover="$3" payload="$4"
    local dir="$WORK/rt128_$tag" embedded pin recovered
    local -a opt=()
    [[ "$option" == "." ]] || opt=("$option")
    mkdir -p "$dir/recover"
    (
        cd "$dir"
        "$BIN" conceal "${opt[@]}" "$cover" "$payload" > conceal.log 2>&1
    )
    embedded="$(extract_embedded_image "$dir/conceal.log")"
    pin="$(extract_pin "$dir/conceal.log")"
    if [[ -z "$embedded" || -z "$pin" || ! -f "$dir/$embedded" ]]; then
        echo "conceal $option did not produce an image and PIN:" >&2
        cat "$dir/conceal.log" >&2
        return 1
    fi
    if [[ ! "$pin" =~ $PIN128_RE ]]; then
        echo "printed PIN is not six groups of four Base58 characters: $pin" >&2
        return 1
    fi
    cp "$dir/$embedded" "$dir/recover/embedded.jpg"
    (
        cd "$dir/recover"
        printf '%s\n' "$pin" | "$BIN" recover embedded.jpg > recover.log 2>&1
    )
    recovered="$(extract_recovered_file "$dir/recover/recover.log")"
    [[ -n "$recovered" && -f "$dir/recover/$recovered" ]] || {
        echo "recovery with the printed PIN failed:" >&2
        cat "$dir/recover/recover.log" >&2
        return 1
    }
    cmp "$payload" "$dir/recover/$recovered"
    RT128_IMAGE="$dir/$embedded"
}

test_roundtrip_default() { roundtrip_pin128 default . "$COVER" "$PAYLOAD"; }
test_roundtrip_bluesky() { roundtrip_pin128 bluesky -b "$COVER" "$PAYLOAD"; }

# Carrier round trip, plus: a numeric PIN selects only the pre-v10 carrier
# formats, so on a v10 carrier it can only give the ordinary carrier message.
roundtrip_carrier_pin128() {
    local tag="$1" option="$2"
    roundtrip_pin128 "$tag" "$option" "$COVER" "$PAYLOAD"
    local dir="$WORK/rt128_${tag}_legacy_pin"
    mkdir -p "$dir"
    cp "$RT128_IMAGE" "$dir/embedded.jpg"
    if (
        cd "$dir"
        printf '%s\n' 15753985819510608748 | "$BIN" recover embedded.jpg > recover.log 2>&1
    ); then
        echo "a numeric PIN recovered a v10 $tag carrier" >&2
        return 1
    fi
    grep -Fq 'File Extraction Error: Invalid KEY/PIN, or this is not a valid jdvrif "file-embedded" image.' \
        "$dir/recover.log"
    assert_no_recovered_payload "$dir"
}

test_roundtrip_reddit() { roundtrip_carrier_pin128 reddit -r; }
test_roundtrip_twitter() { roundtrip_carrier_pin128 twitter -x; }

# recover_default_with <tag> <entered text>: recover $EMBEDDED, expect success.
recover_default_with() {
    local dir="$WORK/pin_io_$1"
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    (
        cd "$dir"
        printf '%s\n' "$2" | "$BIN" recover input.jpg > recover.log 2>&1
    ) || { cat "$dir/recover.log" >&2; return 1; }
    cmp -s "$dir/$(extract_recovered_file "$dir/recover.log")" "$PAYLOAD"
}

# reject_default_with <tag> <entered text> <expected stderr>
reject_default_with() {
    local dir="$WORK/pin_io_$1"
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    if (
        cd "$dir"
        printf '%s\n' "$2" | "$BIN" recover input.jpg > recover.out 2> recover.err
    ); then
        echo "recovery accepted $1 input: $2" >&2
        return 1
    fi
    if [[ "$(error_text "$dir/recover.err")" != "$3" ]]; then
        echo "unexpected error for $1 input:" >&2
        cat "$dir/recover.err" >&2
        return 1
    fi
    assert_no_recovered_payload "$dir"
}

test_pin_io_nodash() { recover_default_with nodash "${PIN//-/}"; }
test_pin_io_spaced() { recover_default_with spaced "${PIN//-/ }"; }

test_pin_io_typo() {
    local last="${PIN: -1}" swap=A
    [[ "$last" != A ]] || swap=B
    reject_default_with typo "${PIN:0:28}$swap" "$PIN_CHECK_ERROR"
}

test_pin_io_malformed() {
    reject_default_with malformed "${PIN:0:28}" "$PIN_FORMAT_ERROR"
}

# Terminal entry: the dash is a stored, echoed keystroke, so typing
# "XXXX-" and one backspace removes the dash, not the fourth character.
test_pin_io_backspace() {
    local dir="$WORK/pin_io_backspace"
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    python3 - "$BIN" "$dir" "$PIN" <<'PY'
import os
import pty
import select
import subprocess
import sys
import termios
import time

binary, work, pin = sys.argv[1:]
master, slave = pty.openpty()
proc = subprocess.Popen([binary, "recover", "input.jpg"], cwd=work, stdin=slave,
                        stdout=slave, stderr=slave, close_fds=True, start_new_session=True)
os.close(slave)
output = bytearray()

def pump(until, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if until():
            return
        ready, _, _ = select.select([master], [], [], 0.05)
        if ready:
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return
            if not chunk:
                return
            output.extend(chunk)
        elif proc.poll() is not None:
            return

def raw_mode():
    try:
        return not (termios.tcgetattr(master)[3] & (termios.ECHO | termios.ICANON))
    except termios.error:
        return False

try:
    pump(lambda: (b"KEY: " in output or b"PIN: " in output) and raw_mode(), timeout=10)
    if not (b"KEY: " in output or b"PIN: " in output):
        raise SystemExit(f"PIN prompt not observed: {bytes(output)!r}")
    keys = pin[:4] + "-" + "\x7f" + "-" + pin[5:] + "\r"
    for key in keys:
        os.write(master, key.encode())
        time.sleep(0.005)
    pump(lambda: proc.poll() is not None)
    status = proc.wait(timeout=60)
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)

text = bytes(output)
echo = text[max(text.find(b"KEY: "), text.find(b"PIN: ")) + 5:]
expected_echo = b"****-\b \b-****-****-****-****-****"
if not echo.startswith(expected_echo):
    raise SystemExit(f"unexpected PIN echo: {echo[:60]!r}")
if pin.encode() in text:
    raise SystemExit("PIN was echoed in the clear")
if status != 0 or b"Complete! Please check your file." not in text:
    raise SystemExit(f"backspaced dash did not recover (status {status}): {text!r}")
PY
    cmp -s "$dir/payload_text.txt" "$PAYLOAD"
}

test_mismatch_legacy_on_v10() {
    local dir="$WORK/mismatch_legacy_on_v10"
    mkdir -p "$dir"
    cp "$EMBEDDED" "$dir/input.jpg"
    if (
        cd "$dir"
        printf '%s\n' 15753985819510608748 | "$BIN" recover input.jpg > recover.out 2> recover.err
    ); then
        echo "a numeric PIN was accepted for a v10 image" >&2
        return 1
    fi
    [[ "$(error_text "$dir/recover.err")" == "$MISMATCH_NEEDS_PIN128" ]] || {
        cat "$dir/recover.err" >&2
        return 1
    }
    assert_no_recovered_payload "$dir"
}

test_mismatch_v10_on_legacy() {
    local dir="$WORK/mismatch_v10_on_legacy" case
    for case in default_single bluesky_single; do
        mkdir -p "$dir/$case"
        cp "$TESTS/golden/$case/embedded.jpg" "$dir/$case/input.jpg"
        if (
            cd "$dir/$case"
            printf '%s\n' "$WRONG_PIN" | "$BIN" recover input.jpg > recover.out 2> recover.err
        ); then
            echo "a 128-bit PIN was accepted for the pre-v10 $case image" >&2
            return 1
        fi
        [[ "$(error_text "$dir/$case/recover.err")" == "$MISMATCH_NEEDS_LEGACY" ]] || {
            cat "$dir/$case/recover.err" >&2
            return 1
        }
        assert_no_recovered_payload "$dir/$case"
    done
}

test_prompt_word_matches_image() {
    # The prompt names what the image needs: KEY for a v10 (KDF5) default or
    # Bluesky image, PIN for a pre-v10 one, and KEY/PIN for a coefficient
    # carrier, which cannot be identified until the key is entered. The entry
    # error uses the same word.
    local dir="$WORK/prompt_word" spec name image expected prompt word heading
    for spec in "v10_default|$EMBEDDED|KEY: " \
                "legacy_default|$TESTS/golden/default_single/embedded.jpg|PIN: " \
                "legacy_bluesky|$TESTS/golden/bluesky_single/embedded.jpg|PIN: " \
                "carrier|$TESTS/testdata/legacy_v9_1/reddit.jpg|KEY/PIN: "; do
        IFS='|' read -r name image expected <<< "$spec"
        mkdir -p "$dir/$name"
        cp "$image" "$dir/$name/input.jpg"
        # Empty input: the prompt is printed, then entry is rejected at once.
        ( cd "$dir/$name" && printf '\n' | "$BIN" recover input.jpg > recover.out 2> recover.err ) || true
        prompt="$(grep -aoE '(KEY/PIN|KEY|PIN): ' "$dir/$name/recover.out" | head -n 1)"
        [[ "$prompt" == "$expected" ]] || {
            echo "$name: expected prompt '$expected', got '$prompt'" >&2
            return 1
        }
        word="${expected%: }"
        heading="$(error_text "$dir/$name/recover.err" | head -n 1)"
        [[ "$heading" == "$word Entry Error: That is not a well-formed recovery $word." ]] || {
            echo "$name: expected a $word entry error, got '$heading'" >&2
            return 1
        }
    done
}

test_legacy_fixtures() {
    bash "$TESTS/run_legacy_fixture_tests.sh" --bin "$BIN" > "$WORK/legacy_fixtures.log" 2>&1 || {
        cat "$WORK/legacy_fixtures.log" >&2
        return 1
    }
    grep -Fxq 'legacy fixtures: 6 passed, 0 failed' "$WORK/legacy_fixtures.log"
}

test_twitter_carrier() {
    local dir="$WORK/twitter_carrier"
    mkdir -p "$dir"
    "${CXX:-g++}" -std=c++23 -O2 -DNDEBUG \
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
        -I"$ROOT" \
        "$TESTS/test_twitter_carrier.cpp" \
        "$ROOT/twitter_steg.cpp" \
        "$ROOT/twitter_jpeg_codec.cpp" \
        "$ROOT/twitter_juniward.cpp" \
        "$ROOT/twitter_stc.cpp" \
        "$ROOT/signal_utils.cpp" \
        "$ROOT/encryption_kdf.cpp" \
        "$ROOT/binary_io.cpp" \
        "$ROOT/file_utils.cpp" \
        -ljpeg -lsodium -o "$dir/test_twitter_carrier"
    "$dir/test_twitter_carrier" "$COVER" "$PAYLOAD" \
        "$TESTS/testdata/legacy_v9_1/twitter.jpg" "$TESTS/testdata/legacy_v9_1/twitter.pin"
}

run_test "build script works outside source directory" test_build_script_from_other_directory
run_test "--info build and Bluesky examples" test_info_documentation
run_test "wrong PIN leaves no plaintext or stages" test_wrong_pin
run_test "default image recovers behind a prepended JFIF segment" test_leading_segments_before_icc
run_test "wrong PIN is rejected without staging the ciphertext" test_wrong_pin_stages_no_ciphertext
run_test "PIN entry rejects stray keystrokes" test_pin_entry_rejects_non_digits
run_test "compression mode is authenticated" test_authenticated_compression_mode
run_test "legacy recovery discloses unauthenticated compression mode" test_legacy_recovery_warning
run_test "recorded KDF costs are range-checked" test_kdf_cost_bounds
run_test "truncated ciphertext is rejected" test_truncated_ciphertext
run_test "default recovery 50 MiB wiggle room" test_default_recovery_wiggle_room
run_test "dangling output symlink is a collision" test_dangling_symlink_collision
run_test "PIN delivery failure does not commit output" test_pin_delivery_failure_is_transactional
run_test "image is committed under its final name before the PIN is shown" test_image_committed_before_pin
run_test "PIN buffers wipe on unwind" test_wipe_guards_unwind
run_test "inflater plaintext buffers wipe on success and failure" test_inflater_wipes_plaintext
run_test "zlib deflate state wipes on release" test_deflater_wipes_plaintext
run_test "input-file readers leave no plaintext in freed heap" test_input_readers_leave_no_plaintext
run_test "pin codec encodes, parses and rejects the shared vectors" test_pin_codec_vectors
run_test "carrier PRF matches SipHash-2-4, libsodium and legacy mix64" test_carrier_prf
run_test "carrier key128 matches the shared Argon2id vector" test_carrier_key128
run_test "Reddit carrier keys RQSTEG3 and routes by key kind" test_reddit_carrier
run_test "X-Twitter carrier keys JXSTEG4 and routes by key kind" test_twitter_carrier
run_test "base64 keeps strict padding, alphabet and rollback" test_base64_wrapper_strictness
run_test "sendfile fallback buffer wipes on release" test_copy_fallback_wipes_plaintext
run_test "staging copy fallback publishes atomically" test_publish_fallback_is_atomic
run_test "JPEG scan limits, cancellation, and exception cleanup" test_jpeg_safety
run_test "carrier read bounds and PIN input signal races" test_io_security
run_test "Bluesky final output is capped at 2,000,000 bytes" test_bluesky_final_output_size_limit
run_test "oversized JPEG dimensions are bounded" test_oversized_jpeg_dimensions
run_test "default cover dimensions are capped at 4096x4096" test_default_dimension_limit
run_test "CMYK covers are rejected" test_cmyk_cover_rejected
run_test "post-transform dimensions are revalidated" test_post_transform_dimensions
run_test "quality limit applies only to default conceal" test_referenced_nonzero_dqt_quality
run_test "terminal state is restored after SIGINT" test_terminal_restored_after_interrupt
run_test "compression stage is anonymous and cleaned on signal" test_signal_cleans_compression_stage
run_test "roundtrip_default: 128-bit PIN printed grouped and recovers" test_roundtrip_default
run_test "roundtrip_bluesky: 128-bit PIN printed grouped and recovers" test_roundtrip_bluesky
run_test "roundtrip_reddit: RQSTEG3 recovers; numeric PIN gets carrier message" test_roundtrip_reddit
run_test "roundtrip_twitter: JXSTEG4 recovers; numeric PIN gets carrier message" test_roundtrip_twitter
run_test "pin_io_nodash: PIN without dashes recovers" test_pin_io_nodash
run_test "pin_io_spaced: PIN with spaces for dashes recovers" test_pin_io_spaced
run_test "pin_io_typo: changed last character is a checksum error" test_pin_io_typo
run_test "pin_io_malformed: 23 characters is a format error" test_pin_io_malformed
run_test "pin_io_backspace: backspace after a dash removes the dash" test_pin_io_backspace
run_test "mismatch_legacy_on_v10: numeric PIN on a KDF5 image" test_mismatch_legacy_on_v10
run_test "mismatch_v10_on_legacy: 128-bit PIN on a pre-v10 image" test_mismatch_v10_on_legacy
run_test "legacy_fixtures: pre-v10 carriers recover with decimal PINs" test_legacy_fixtures
run_test "prompt says KEY, PIN or KEY/PIN to match the image" test_prompt_word_matches_image

echo
echo "Security smoke summary: PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
