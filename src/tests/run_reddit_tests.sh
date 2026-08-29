#!/bin/bash
# Reddit conceal-path format, boundary, and failure-path tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TESTS="$ROOT/tests"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_reddit_tests.sh [options]

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

need_cmd find
need_cmd grep
need_cmd cmp
need_cmd python3
need_cmd sed
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

COVER="$TESTS/testdata/covers/cover_default.jpg"
PAYLOAD="$TESTS/testdata/payloads/payload_text.txt"
LARGE_PAYLOAD="$TESTS/testdata/payloads/bsingle.bin"
if [[ ! -f "$COVER" || ! -f "$PAYLOAD" || ! -f "$LARGE_PAYLOAD" ]]; then
    bash "$TESTS/create_testdata.sh" >/dev/null
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-reddit.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
PASS=0

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" JPG image: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

assert_no_output_image() {
    local dir="$1"
    if find "$dir" -maxdepth 1 -type f -name 'jrif_*.jpg' -print -quit | grep -q .; then
        echo "unexpected Reddit output image" >&2
        return 1
    fi
}

# `capsize -r` must run the exact Reddit normalization/Q75 carrier preparation,
# account for the encrypted envelope and a 20-character filename, and save no
# image.
mkdir "$WORK/capsize"
cp "$COVER" "$WORK/capsize/cover.jpg"
(
    cd "$WORK/capsize"
    "$BIN" capsize -r cover.jpg > capsize.log 2>&1
)
CAPSIZE_COVER_BYTES="$(stat -c '%s' "$WORK/capsize/cover.jpg")"
CAPSIZE_COVER_KIB="$((CAPSIZE_COVER_BYTES / 1024))"
grep -Fq 'Reddit capacity check for conceal -r mode only.' "$WORK/capsize/capsize.log"
grep -Fq "Cover Image: ${CAPSIZE_COVER_KIB}KiB, 1280x960, Baseline YCbCr 4:2:0, Standard Q75 quantization (C3)." "$WORK/capsize/capsize.log"
grep -Eq '^Theoretical C3 capacity limit for this cover image: +8000 bytes \(~7KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Eq '^Conservative maximum compressed capacity with a 20-character filename: +7886 bytes \(~7KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Eq '^Recommended  maximum compressed capacity with a 20-character filename: +6862 bytes \(~6KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Fq 'consume 95 to 114 bytes' "$WORK/capsize/capsize.log"
grep -Fq 'final conceal -r size check is authoritative' "$WORK/capsize/capsize.log"
grep -Fq 'must remain within 20MB.' "$WORK/capsize/capsize.log"
assert_no_output_image "$WORK/capsize"
PASS=$((PASS + 1))

# Successful conceal and exact carrier-format validation.
mkdir "$WORK/success"
cp "$COVER" "$WORK/success/cover.jpg"
cp "$PAYLOAD" "$WORK/success/payload.txt"
(
    cd "$WORK/success"
    "$BIN" conceal -r cover.jpg payload.txt > conceal.log 2>&1
)
OUTPUT_NAME="$(extract_embedded_image "$WORK/success/conceal.log")"
if [[ -z "$OUTPUT_NAME" || ! -f "$WORK/success/$OUTPUT_NAME" ]]; then
    echo "Reddit conceal did not report/create its output image" >&2
    exit 1
fi
grep -Fq 'Reddit. (Only share this "file-embedded" JPG image on Reddit).' "$WORK/success/conceal.log"
grep -Eq 'Recovery PIN: \[\*\*\*[0-9]+\*\*\*\]' "$WORK/success/conceal.log"
if grep -Fq 'Reddit carrier:' "$WORK/success/conceal.log"; then
    echo "successful Reddit conceal unexpectedly displayed capacity diagnostics" >&2
    exit 1
fi

python3 - "$WORK/success/$OUTPUT_NAME" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if not data.startswith(b"\xff\xd8"):
    raise SystemExit("Reddit output lacks JPEG SOI")

zigzag_to_natural = [
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
]
luma = [
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99,
]
chroma = [
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
]

def q75(base):
    natural = [max(1, min(255, (value * 50 + 50) // 100)) for value in base]
    return bytes(natural[index] for index in zigzag_to_natural)

tables = {}
frame = None
app_markers = []
pos = 2
while pos < len(data):
    if data[pos] != 0xFF:
        raise SystemExit(f"bad JPEG marker at {pos}")
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    marker = data[pos]
    pos += 1
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    if pos + 2 > len(data):
        raise SystemExit("truncated JPEG segment")
    length = int.from_bytes(data[pos:pos + 2], "big")
    end = pos + length
    if length < 2 or end > len(data):
        raise SystemExit("invalid JPEG segment length")
    payload = data[pos + 2:end]
    if 0xE0 <= marker <= 0xEF:
        app_markers.append(marker)
    if marker == 0xDB:
        cursor = 0
        while cursor < len(payload):
            spec = payload[cursor]
            cursor += 1
            precision, table_id = spec >> 4, spec & 0x0F
            if precision != 0 or cursor + 64 > len(payload):
                raise SystemExit("unexpected Reddit DQT encoding")
            tables[table_id] = payload[cursor:cursor + 64]
            cursor += 64
    elif marker in range(0xC0, 0xD0) and marker not in (0xC4, 0xC8, 0xCC):
        if marker != 0xC0:
            raise SystemExit(f"Reddit output is not baseline SOF0 (marker ff{marker:02x})")
        components = payload[5]
        values = []
        cursor = 6
        for _ in range(components):
            values.append((payload[cursor], payload[cursor + 1], payload[cursor + 2]))
            cursor += 3
        frame = values
    if marker == 0xDA:
        break
    pos = end

if app_markers:
    raise SystemExit(f"Reddit output contains metadata APP markers: {app_markers}")
if frame != [(1, 0x22, 0), (2, 0x11, 1), (3, 0x11, 1)]:
    raise SystemExit(f"unexpected Reddit component/subsampling layout: {frame}")
if tables.get(0) != q75(luma) or tables.get(1) != q75(chroma):
    raise SystemExit("Reddit output does not contain the standard Q75 tables")
PY

# Recover through the DCT route, existing PIN/KDF/secretstream decryptor, and
# streaming zlib inflater, then compare the final plaintext byte-for-byte.
PIN="$(extract_pin "$WORK/success/conceal.log")"
if [[ -z "$PIN" ]]; then
    echo "Reddit conceal did not report a recovery PIN" >&2
    exit 1
fi
mkdir "$WORK/recover"
cp "$WORK/success/$OUTPUT_NAME" "$WORK/recover/embedded.jpg"
(
    cd "$WORK/recover"
    printf '%s\n' "$PIN" | "$BIN" recover embedded.jpg > recover.log 2>&1
)
RECOVERED_NAME="$(extract_recovered_file "$WORK/recover/recover.log")"
if [[ -z "$RECOVERED_NAME" || ! -f "$WORK/recover/$RECOVERED_NAME" ]]; then
    echo "Reddit recovery did not report/create its decrypted output" >&2
    exit 1
fi
cmp "$PAYLOAD" "$WORK/recover/$RECOVERED_NAME"
grep -Fq 'Complete! Please check your file.' "$WORK/recover/recover.log"
PASS=$((PASS + 1))

# Recognized already-compressed formats bypass zlib in -r mode regardless of
# size. This 7,890-byte fixture fits the C3 carrier raw, while wrapping it in
# zlib would exceed the same carrier after encryption/envelope overhead.
mkdir "$WORK/compressed_bypass"
cp "$COVER" "$WORK/compressed_bypass/cover.jpg"
python3 - "$WORK/compressed_bypass/payload.zip" <<'PY'
import hashlib
import sys
import zlib
from pathlib import Path

payload = b"".join(hashlib.sha256(i.to_bytes(4, "big")).digest() for i in range(247))[:7890]
if len(zlib.compress(payload, 6)) <= 7895:
    raise SystemExit("compressed-bypass fixture unexpectedly shrank enough to fit")
Path(sys.argv[1]).write_bytes(payload)
PY
(
    cd "$WORK/compressed_bypass"
    "$BIN" conceal -r cover.jpg payload.zip > conceal.log 2>&1
)
BYPASS_OUTPUT="$(extract_embedded_image "$WORK/compressed_bypass/conceal.log")"
BYPASS_PIN="$(extract_pin "$WORK/compressed_bypass/conceal.log")"
if [[ -z "$BYPASS_OUTPUT" || -z "$BYPASS_PIN" || ! -f "$WORK/compressed_bypass/$BYPASS_OUTPUT" ]]; then
    echo "Reddit did not accept the below-10-MiB already-compressed payload without recompression" >&2
    exit 1
fi
mkdir "$WORK/compressed_bypass_recover"
cp "$WORK/compressed_bypass/$BYPASS_OUTPUT" "$WORK/compressed_bypass_recover/embedded.jpg"
(
    cd "$WORK/compressed_bypass_recover"
    printf '%s\n' "$BYPASS_PIN" | "$BIN" recover embedded.jpg > recover.log 2>&1
)
BYPASS_RECOVERED="$(extract_recovered_file "$WORK/compressed_bypass_recover/recover.log")"
if [[ -z "$BYPASS_RECOVERED" || ! -f "$WORK/compressed_bypass_recover/$BYPASS_RECOVERED" ]]; then
    echo "Reddit did not recover the uncompressed carrier payload" >&2
    exit 1
fi
cmp "$WORK/compressed_bypass/payload.zip" "$WORK/compressed_bypass_recover/$BYPASS_RECOVERED"
PASS=$((PASS + 1))

# A wrong PIN must not create plaintext or leave anonymous-stage fallbacks.
mkdir "$WORK/wrong_pin"
cp "$WORK/success/$OUTPUT_NAME" "$WORK/wrong_pin/embedded.jpg"
WRONG_PIN=1
if [[ "$PIN" == "$WRONG_PIN" ]]; then
    WRONG_PIN=2
fi
if (
    cd "$WORK/wrong_pin"
    printf '%s\n' "$WRONG_PIN" | "$BIN" recover embedded.jpg > recover.log 2>&1
); then
    echo "Reddit recovery unexpectedly accepted a wrong PIN" >&2
    exit 1
fi
# The carrier's coefficient positions come from the PIN, so a wrong one fails at
# carrier location rather than at decryption -- and reports the same thing an
# ordinary JPEG does. That indistinguishability is the point of keying it.
grep -Fq 'Invalid PIN, or this is not a valid jdvrif "file-embedded" image' "$WORK/wrong_pin/recover.log"
if find "$WORK/wrong_pin" -maxdepth 1 -type f -name 'payload*.txt' -print -quit | grep -q .; then
    echo "wrong-PIN Reddit recovery wrote plaintext" >&2
    exit 1
fi
PASS=$((PASS + 1))

# A normal JPEG with no APP/EXIF jdvrif signature is rejected as non-jdvrif.
#
# It now reaches the PIN prompt first, and must give the *same* message a wrong
# PIN on a real carrier gives: without the PIN there is no way to tell the two
# apart, and the tool must not leak the difference.
mkdir "$WORK/not_embedded"
cp "$COVER" "$WORK/not_embedded/image.jpg"
if (
    cd "$WORK/not_embedded"
    printf '%s\n' "$WRONG_PIN" | "$BIN" recover image.jpg > recover.log 2>&1
); then
    echo "ordinary JPEG was unexpectedly accepted as a jdvrif image" >&2
    exit 1
fi
grep -Fq 'Invalid PIN, or this is not a valid jdvrif "file-embedded" image' "$WORK/not_embedded/recover.log"
# Same wording for both, byte for byte.
if ! diff -q \
    <(grep -F 'Invalid PIN, or this is not' "$WORK/wrong_pin/recover.log") \
    <(grep -F 'Invalid PIN, or this is not' "$WORK/not_embedded/recover.log") >/dev/null; then
    echo "wrong-PIN and ordinary-JPEG rejections differ; the carrier is distinguishable" >&2
    exit 1
fi
PASS=$((PASS + 1))

# Reddit's input ceiling replaces (rather than inheriting) the normal 8 MiB
# cover-read ceiling.  Trailing JPEG padding is discarded by the transcode.
mkdir "$WORK/cover_over_default"
cp "$COVER" "$WORK/cover_over_default/cover.jpg"
cp "$PAYLOAD" "$WORK/cover_over_default/payload.txt"
python3 - "$WORK/cover_over_default/cover.jpg" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.truncate(8 * 1024 * 1024 + 1)
PY
(
    cd "$WORK/cover_over_default"
    "$BIN" conceal -r cover.jpg payload.txt > conceal.log 2>&1
)
PADDED_OUTPUT="$(extract_embedded_image "$WORK/cover_over_default/conceal.log")"
if [[ -z "$PADDED_OUTPUT" || ! -f "$WORK/cover_over_default/$PADDED_OUTPUT" ]]; then
    echo "Reddit conceal incorrectly retained the default 8 MiB cover limit" >&2
    exit 1
fi
PASS=$((PASS + 1))

# Compressed data that cannot fit must report the cover, stored payload, and
# theoretical/conservative/recommended limits.
mkdir "$WORK/capacity"
cp "$COVER" "$WORK/capacity/cover.jpg"
cp "$LARGE_PAYLOAD" "$WORK/capacity/large.bin"
if (cd "$WORK/capacity" && "$BIN" conceal -r cover.jpg large.bin > conceal.log 2>&1); then
    echo "oversized compressed Reddit payload was unexpectedly accepted" >&2
    exit 1
fi
CAPACITY_COVER_BYTES="$(stat -c '%s' "$WORK/capacity/cover.jpg")"
CAPACITY_COVER_KIB="$((CAPACITY_COVER_BYTES / 1024))"
grep -Fq "Cover Image: ${CAPACITY_COVER_KIB}KiB, 1280x960, Baseline YCbCr 4:2:0, Standard Q75 quantization (C3)." "$WORK/capacity/conceal.log"
grep -Fq 'Compressed data file (payload) size: 62011 bytes (60KiB).' "$WORK/capacity/conceal.log"
grep -Eq '^Theoretical C3 capacity limit for this cover image: +8000 bytes \(~7KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Eq '^Conservative maximum compressed capacity with a 20-character filename: +7886 bytes \(~7KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Eq '^Recommended  maximum compressed capacity with a 20-character filename: +6862 bytes \(~6KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Fq 'Data File Size Error:' "$WORK/capacity/conceal.log"
grep -Fq 'Already-compressed payload file size of 62011 bytes (60KiB) exceeds the recommended maximum limit of 6862 bytes (~6KiB) for this cover image.' "$WORK/capacity/conceal.log"
assert_no_output_image "$WORK/capacity"
PASS=$((PASS + 1))

# Both raw inputs have an independent 20 MiB preliminary ceiling.
mkdir "$WORK/raw_payload"
cp "$COVER" "$WORK/raw_payload/cover.jpg"
cp "$PAYLOAD" "$WORK/raw_payload/payload.bin"
python3 - "$WORK/raw_payload/payload.bin" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.truncate(20 * 1024 * 1024 + 1)
PY
if (cd "$WORK/raw_payload" && "$BIN" conceal -r cover.jpg payload.bin > conceal.log 2>&1); then
    echo "over-20-MiB Reddit payload was unexpectedly accepted" >&2
    exit 1
fi
grep -Fq "Payload file exceeds Reddit's 20 MiB upload size limit" "$WORK/raw_payload/conceal.log"
assert_no_output_image "$WORK/raw_payload"
PASS=$((PASS + 1))

mkdir "$WORK/raw_cover"
cp "$COVER" "$WORK/raw_cover/cover.jpg"
cp "$PAYLOAD" "$WORK/raw_cover/payload.txt"
python3 - "$WORK/raw_cover/cover.jpg" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.truncate(20 * 1024 * 1024 + 1)
PY
if (cd "$WORK/raw_cover" && "$BIN" conceal -r cover.jpg payload.txt > conceal.log 2>&1); then
    echo "over-20-MiB Reddit cover was unexpectedly accepted" >&2
    exit 1
fi
grep -Fq "Cover image exceeds Reddit's 20 MiB upload size limit" "$WORK/raw_cover/conceal.log"
assert_no_output_image "$WORK/raw_cover"
PASS=$((PASS + 1))

echo "Reddit conceal/recovery tests passed: $PASS"
