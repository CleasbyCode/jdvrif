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
REDDIT_COVER="$TESTS/testdata/covers/cover_reddit.jpg"
PAYLOAD="$TESTS/testdata/payloads/payload_text.txt"

if [[ ! -f "$COVER" || ! -f "$REDDIT_COVER" || ! -f "$PAYLOAD" ]]; then
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
    if (set -euo pipefail; "$@"); then
        echo "[PASS] $name"
        PASS=$((PASS + 1))
    else
        rc=$?
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

prepare_fresh_reddit_fixture() {
    local dir="$WORK/fresh_reddit"
    mkdir -p "$dir"
    (
        cd "$dir"
        "$BIN" conceal -r "$REDDIT_COVER" "$PAYLOAD" > conceal.log 2>&1
    )

    local embedded_name
    embedded_name="$(extract_embedded_image "$dir/conceal.log")"
    REDDIT_PIN="$(extract_pin "$dir/conceal.log")"
    if [[ -z "$embedded_name" || -z "$REDDIT_PIN" || ! -f "$dir/$embedded_name" ]]; then
        echo "Failed to create fresh Reddit fixture:" >&2
        cat "$dir/conceal.log" >&2
        exit 1
    fi
    REDDIT_EMBEDDED="$dir/$embedded_name"
}

prepare_fresh_fixture
prepare_fresh_reddit_fixture

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
    grep -q 'CMake 3.20' "$info"
    grep -q 'ninja-build' "$info"
    grep -q 'binutils' "$info"
    grep -q 'zlib1g-dev' "$info"
    grep -q 'libdeflate-dev' "$info"
    grep -q 'bsky/bsky_post.py' "$info"
    grep -q 'ATP_AUTH_PASSWORD' "$info"
    grep -q 'alt text for image 1' "$info"
    grep -q 'alt text for image 2' "$info"
    grep -Fq 'python3 bsky/bsky_post.py \' "$info"
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

# The version marker is mutable metadata too. Downgrading KDF3 to legacy KDF2
# must not remove the associated-data check and resurrect the old ambiguity.
kdf = base + 0x2FB
if data[kdf:kdf + 4] != b"KDF3":
    raise SystemExit("fresh fixture did not use authenticated KDF3 framing")
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

test_reddit_recovery_wiggle_room() {
    assert_size_boundary_behavior \
        "$REDDIT_EMBEDDED" "$REDDIT_PIN" "$((20 * 1024 * 1024 + 2 * 1024 * 1024))" \
        reddit_size_boundary
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
    if command -v timeout >/dev/null 2>&1; then
        set +e
        if [[ -n "${ASAN_OPTIONS:-}" ]]; then
            (
                cd "$dir"
                timeout 10 "$BIN" conceal huge.jpg "$PAYLOAD" > conceal.log 2>&1
            )
        else
            (
                cd "$dir"
                ulimit -v 524288
                timeout 10 "$BIN" conceal huge.jpg "$PAYLOAD" > conceal.log 2>&1
            )
        fi
        rc=$?
        set -e
        if [[ "$rc" -eq 0 || "$rc" -eq 124 || "$rc" -eq 137 ]]; then
            echo "oversized-dimension rejection returned unsafe status $rc" >&2
            return 1
        fi
    else
        if [[ -n "${ASAN_OPTIONS:-}" ]]; then
            if (
                cd "$dir"
                "$BIN" conceal huge.jpg "$PAYLOAD" > conceal.log 2>&1
            ); then
                echo "oversized JPEG dimensions were accepted" >&2
                return 1
            fi
        elif (
            cd "$dir"
            ulimit -v 524288
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
        if find "$dir" -maxdepth 1 -type f -name '.jdvrif_comp_*' -print -quit | grep -q .; then
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
        echo "did not observe the compression staging file before process exit" >&2
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

run_test "build script works outside source directory" test_build_script_from_other_directory
run_test "--info build and Bluesky examples" test_info_documentation
run_test "wrong PIN leaves no plaintext or stages" test_wrong_pin
run_test "compression mode is authenticated" test_authenticated_compression_mode
run_test "truncated ciphertext is rejected" test_truncated_ciphertext
run_test "default recovery 50 MiB wiggle room" test_default_recovery_wiggle_room
run_test "Reddit recovery 2 MiB wiggle room" test_reddit_recovery_wiggle_room
run_test "dangling output symlink is a collision" test_dangling_symlink_collision
run_test "PIN delivery failure does not commit output" test_pin_delivery_failure_is_transactional
run_test "oversized JPEG dimensions are bounded" test_oversized_jpeg_dimensions
run_test "CMYK covers are rejected" test_cmyk_cover_rejected
run_test "post-transform dimensions are revalidated" test_post_transform_dimensions
run_test "quality follows referenced nonzero DQT" test_referenced_nonzero_dqt_quality
run_test "terminal state is restored after SIGINT" test_terminal_restored_after_interrupt
run_test "signal interruption cleans compression stage" test_signal_cleans_compression_stage

echo
echo "Security smoke summary: PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
