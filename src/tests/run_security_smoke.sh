#!/bin/bash
# Focused negative-path and boundary regression tests for jdvrif.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TESTS="$ROOT/tests"
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
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
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
    if grep -q -- '--password xxxx' "$info"; then
        echo "info text still recommends exposing an app password on the command line" >&2
        return 1
    fi
}

test_wrong_pin() {
    local dir="$WORK/wrong_pin"
    local wrong_pin=0
    if [[ "$PIN" == "0" ]]; then wrong_pin=1; fi
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
# (KDF3, KDF4, ...) so retiring a mint version stays a one-line change here.
kdf = base + 0x2FB
authenticated = (b"KDF3", b"KDF4")
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

# KDF4 records the Argon2id cost parameters in the image. Out-of-range costs
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
if bytes(data[kdf:kdf + 4]) != b"KDF4":
    raise SystemExit(f"fixture is not KDF4: {bytes(data[kdf:kdf + 4])!r}")

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
        if grep -Fq 'PIN:' "$dir/recover-$image.log"; then
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
    if ! grep -q 'PIN:' "$dir/accepted/recover.log"; then
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
    if grep -q 'PIN:' "$dir/rejected/recover.log"; then
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
    if grep -Fq 'Recovery PIN:' "$dir/rejected/conceal.log"; then
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
    while b"PIN: " not in output and time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                output.extend(os.read(master, 4096))
            except OSError:
                break
        if proc.poll() is not None:
            break
    if b"PIN: " not in output:
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
    "${CXX:-g++}" -std=c++23 -I"$ROOT" "$ROOT/tests/test_wipe_guards.cpp" -lsodium -o "$dir/test_wipe_guards"
    "$dir/test_wipe_guards"
}

# After the recovery PIN has been printed, the fsynced temp image is the only
# durable copy that PIN can open. A later commit failure (here: colliding the
# chosen output name) must leave that image in place and name it in the error.
test_commit_collision_after_pin_keeps_image() {
    local dir="$WORK/commit_collision"
    mkdir -p "$dir"
    python3 - "$BIN" "$COVER" "$PAYLOAD" "$dir" <<'PY'
import ctypes
import os
import struct
import subprocess
import sys
from pathlib import Path

binary, cover, payload, work = sys.argv[1:]
work = Path(work)

libc = ctypes.CDLL("libc.so.6", use_errno=True)
IN_CREATE = 0x00000100
IN_MOVED_TO = 0x00000080
EVENT = struct.Struct("iIII")

fd = libc.inotify_init1(os.O_CLOEXEC)
if fd < 0:
    raise OSError(ctypes.get_errno(), "inotify_init1")
watch = libc.inotify_add_watch(fd, os.fsencode(work), IN_CREATE | IN_MOVED_TO)
if watch < 0:
    os.close(fd)
    raise OSError(ctypes.get_errno(), "inotify_add_watch")

proc = subprocess.Popen(
    [binary, "conceal", cover, payload],
    cwd=work,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

temp = None
try:
    leftover = b""
    while temp is None:
        if proc.poll() is not None:
            break
        chunk = leftover + os.read(fd, 4096)
        leftover = b""
        pos = 0
        while pos + EVENT.size <= len(chunk):
            wd, mask, cookie, length = EVENT.unpack_from(chunk, pos)
            pos += EVENT.size
            if pos + length > len(chunk):
                leftover = chunk[pos - EVENT.size:]
                break
            name = chunk[pos:pos + length].split(b"\x00", 1)[0].decode()
            pos += length
            if ".jdvrif_tmp_" not in name or not name.startswith(".jrif_"):
                continue
            temp = work / name
            marker = ".jdvrif_tmp_"
            output_name = name[1:name.find(marker)]
            (work / output_name).write_bytes(b"collision")
            break
    stdout, stderr = proc.communicate(timeout=30)
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(fd)

log = stdout + "\n" + stderr
if temp is None:
    raise RuntimeError(f"did not observe staged output before conceal exited:\n{log}")
if proc.returncode == 0:
    raise RuntimeError("conceal succeeded despite an output-name collision")
if "Recovery PIN:" not in log:
    raise RuntimeError(f"PIN was not delivered before commit failure:\n{log}")
if not temp.exists():
    raise RuntimeError("durable staged image was deleted after PIN delivery")
if temp.name not in log and str(temp) not in log:
    raise RuntimeError(f"error did not mention leftover image path:\n{log}")
PY
}

run_test "build script works outside source directory" test_build_script_from_other_directory
run_test "--info build and Bluesky examples" test_info_documentation
run_test "wrong PIN leaves no plaintext or stages" test_wrong_pin
run_test "compression mode is authenticated" test_authenticated_compression_mode
run_test "recorded KDF costs are range-checked" test_kdf_cost_bounds
run_test "truncated ciphertext is rejected" test_truncated_ciphertext
run_test "default recovery 50 MiB wiggle room" test_default_recovery_wiggle_room
run_test "dangling output symlink is a collision" test_dangling_symlink_collision
run_test "PIN delivery failure does not commit output" test_pin_delivery_failure_is_transactional
run_test "post-PIN commit failure keeps durable image" test_commit_collision_after_pin_keeps_image
run_test "PIN buffers wipe on unwind" test_wipe_guards_unwind
run_test "Bluesky final output is capped at 2,000,000 bytes" test_bluesky_final_output_size_limit
run_test "oversized JPEG dimensions are bounded" test_oversized_jpeg_dimensions
run_test "CMYK covers are rejected" test_cmyk_cover_rejected
run_test "post-transform dimensions are revalidated" test_post_transform_dimensions
run_test "quality limit applies only to default conceal" test_referenced_nonzero_dqt_quality
run_test "terminal state is restored after SIGINT" test_terminal_restored_after_interrupt
run_test "compression stage is anonymous and cleaned on signal" test_signal_cleans_compression_stage

echo
echo "Security smoke summary: PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
