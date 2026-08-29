#!/bin/bash
# X-Twitter adaptive conceal-path format, carrier, and boundary tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TESTS="$ROOT/tests"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_twitter_tests.sh [options]

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
need_cmd find
need_cmd grep
need_cmd python3
need_cmd sed
need_cmd stat
need_cmd "${CXX:-g++}"

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
ALT_COVER="$TESTS/testdata/covers/cover_tables.jpg"
PAYLOAD="$TESTS/testdata/payloads/payload_text.txt"
LARGE_PAYLOAD="$TESTS/testdata/payloads/bsingle.bin"
if [[ ! -f "$COVER" || ! -f "$ALT_COVER" || ! -f "$PAYLOAD" || ! -f "$LARGE_PAYLOAD" ]]; then
    bash "$TESTS/create_testdata.sh" >/dev/null
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-twitter.XXXXXX")"
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
        echo "unexpected X-Twitter output image after failed conceal" >&2
        return 1
    fi
}

# The low-level carrier must round-trip arbitrary bytes under the same key and
# must not be locatable with another key. This validates the format primitive
# shared by the conceal and recovery routes.
"${CXX:-g++}" \
    -std=c++23 -O2 -DNDEBUG \
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -I"$ROOT" \
    "$TESTS/test_twitter_carrier.cpp" \
    "$ROOT/twitter_steg.cpp" \
    "$ROOT/twitter_jpeg_codec.cpp" \
    "$ROOT/twitter_juniward.cpp" \
    "$ROOT/twitter_stc.cpp" \
    "$ROOT/signal_utils.cpp" \
    -ljpeg -lsodium \
    -o "$WORK/test_twitter_carrier"
"$WORK/test_twitter_carrier" "$COVER" "$PAYLOAD"
PASS=$((PASS + 1))

# `capsize` must run the same source-quality carrier preparation as conceal -x,
# report both envelope and conservative compressed-data limits, and save no
# image. The Q65 fixture has a deliberately small but deterministic capacity.
mkdir "$WORK/capsize"
cp "$COVER" "$WORK/capsize/cover.jpg"
(
    cd "$WORK/capsize"
    "$BIN" capsize -x cover.jpg > capsize.log 2>&1
)
grep -Fq 'X-Twitter capacity check for conceal -x mode only.' "$WORK/capsize/capsize.log"
grep -Eq '^Cover Image: [0-9]+KiB, 1280x960, Progressive YCbCr 4:2:0, Source-derived Q65 quantization \(J-UNIWARD/STC\)\.$' "$WORK/capsize/capsize.log"
grep -Eq '^Theoretical J-UNIWARD/STC capacity limit for this cover image: +739 bytes \(~0KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Eq '^Conservative maximum compressed capacity with a 20-character filename: +625 bytes \(~0KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Eq '^Recommended  maximum compressed capacity with a 20-character filename: +0 bytes \(~0KiB\)\.$' "$WORK/capsize/capsize.log"
grep -Fq 'consume 95 to 114 bytes' "$WORK/capsize/capsize.log"
grep -Fq 'at least' "$WORK/capsize/capsize.log"
grep -Fq '1KiB below' "$WORK/capsize/capsize.log"
assert_no_output_image "$WORK/capsize"
PASS=$((PASS + 1))

# Successful CLI conceal and exact transport-format validation.
mkdir "$WORK/success"
cp "$COVER" "$WORK/success/cover.jpg"
cp "$PAYLOAD" "$WORK/success/payload.txt"
(
    cd "$WORK/success"
    "$BIN" conceal -x cover.jpg payload.txt > conceal.log 2>&1
)
OUTPUT_NAME="$(extract_embedded_image "$WORK/success/conceal.log")"
if [[ -z "$OUTPUT_NAME" || ! -f "$WORK/success/$OUTPUT_NAME" ]]; then
    echo "X-Twitter conceal did not report/create its output image" >&2
    exit 1
fi
grep -Fq 'X-Twitter. (Only share this "file-embedded" JPG image on X-Twitter).' "$WORK/success/conceal.log"
grep -Eq 'Recovery PIN: \[\*\*\*[0-9]+\*\*\*\]' "$WORK/success/conceal.log"
if grep -Fq 'X-Twitter carrier:' "$WORK/success/conceal.log"; then
    echo "successful X-Twitter conceal unexpectedly displayed capacity diagnostics" >&2
    exit 1
fi

python3 - "$WORK/success/$OUTPUT_NAME" "$WORK/success/cover.jpg" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
source_data = Path(sys.argv[2]).read_bytes()
jfif = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
if not data.startswith(jfif):
    raise SystemExit("X-Twitter output lacks the standard JFIF 1.01 header")
if len(data) > 5 * 1024 * 1024:
    raise SystemExit("X-Twitter output exceeds 5 MiB")

def parse_header(jpeg):
    tables = {}
    frame = None
    frame_marker = None
    apps = []
    comments = 0
    pos = 2
    while pos < len(jpeg):
        if jpeg[pos] != 0xFF:
            raise SystemExit(f"bad JPEG marker at {pos}")
        while pos < len(jpeg) and jpeg[pos] == 0xFF:
            pos += 1
        marker = jpeg[pos]
        pos += 1
        if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        length = int.from_bytes(jpeg[pos:pos + 2], "big")
        end = pos + length
        if length < 2 or end > len(jpeg):
            raise SystemExit("invalid JPEG segment length")
        payload = jpeg[pos + 2:end]
        if 0xE0 <= marker <= 0xEF:
            apps.append(marker)
        elif marker == 0xFE:
            comments += 1
        elif marker == 0xDB:
            cursor = 0
            while cursor < len(payload):
                spec = payload[cursor]
                cursor += 1
                precision, table_id = spec >> 4, spec & 0x0F
                table_bytes = 64 if precision == 0 else 128
                if precision > 1 or cursor + table_bytes > len(payload):
                    raise SystemExit("unexpected X-Twitter DQT encoding")
                tables[table_id] = (precision, payload[cursor:cursor + table_bytes])
                cursor += table_bytes
        elif marker in range(0xC0, 0xD0) and marker not in (0xC4, 0xC8, 0xCC):
            frame_marker = marker
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
    return tables, frame, frame_marker, apps, comments

tables, frame, frame_marker, apps, comments = parse_header(data)
source_tables, source_frame, _, _, _ = parse_header(source_data)

if apps != [0xE0]:
    raise SystemExit(f"unexpected X-Twitter APP markers: {apps}")
if comments:
    raise SystemExit("X-Twitter output retained JPEG comments")
if frame_marker != 0xC2:
    raise SystemExit(f"X-Twitter output is not progressive SOF2 (ff{frame_marker:02x})")
if frame is None or [(entry[0], entry[1]) for entry in frame] != [
    (1, 0x22), (2, 0x11), (3, 0x11)
]:
    raise SystemExit(f"unexpected component/subsampling layout: {frame}")
if source_frame is None or len(source_frame) != 3:
    raise SystemExit("test source has an unexpected component layout")
for component in range(3):
    output_table = tables.get(frame[component][2])
    source_table = source_tables.get(source_frame[component][2])
    if output_table is None or output_table != source_table:
        raise SystemExit("X-Twitter output did not preserve source quantization tables")
PY
PASS=$((PASS + 1))

# Recover through the keyed progressive carrier, compact KDF envelope,
# authenticated secretstream decryptor, and streaming zlib inflater.
PIN="$(extract_pin "$WORK/success/conceal.log")"
if [[ -z "$PIN" ]]; then
    echo "X-Twitter conceal did not report a recovery PIN" >&2
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
    echo "X-Twitter recovery did not report/create its decrypted output" >&2
    exit 1
fi
cmp "$PAYLOAD" "$WORK/recover/$RECOVERED_NAME"
grep -Fq 'Complete! Please check your file.' "$WORK/recover/recover.log"
PASS=$((PASS + 1))

# Recognized already-compressed formats bypass zlib in -x mode regardless of
# size. This 630-byte fixture fits the carrier raw, while wrapping it in zlib
# would exceed the same carrier after encryption/envelope overhead.
mkdir "$WORK/compressed_bypass"
cp "$COVER" "$WORK/compressed_bypass/cover.jpg"
python3 - "$WORK/compressed_bypass/payload.zip" <<'PY'
import hashlib
import sys
import zlib
from pathlib import Path

payload = b"".join(hashlib.sha256(i.to_bytes(4, "big")).digest() for i in range(20))[:630]
if len(zlib.compress(payload, 6)) <= 634:
    raise SystemExit("compressed-bypass fixture unexpectedly shrank enough to fit")
Path(sys.argv[1]).write_bytes(payload)
PY
(
    cd "$WORK/compressed_bypass"
    "$BIN" conceal -x cover.jpg payload.zip > conceal.log 2>&1
)
BYPASS_OUTPUT="$(extract_embedded_image "$WORK/compressed_bypass/conceal.log")"
BYPASS_PIN="$(extract_pin "$WORK/compressed_bypass/conceal.log")"
if [[ -z "$BYPASS_OUTPUT" || -z "$BYPASS_PIN" || ! -f "$WORK/compressed_bypass/$BYPASS_OUTPUT" ]]; then
    echo "X-Twitter did not accept the below-10-MiB already-compressed payload without recompression" >&2
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
    echo "X-Twitter did not recover the uncompressed carrier payload" >&2
    exit 1
fi
cmp "$WORK/compressed_bypass/payload.zip" "$WORK/compressed_bypass_recover/$BYPASS_RECOVERED"
PASS=$((PASS + 1))

# Carrier position/key failure must not create plaintext, and must remain
# indistinguishable from a normal non-jdvrif progressive JPEG.
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
    echo "X-Twitter recovery unexpectedly accepted a wrong PIN" >&2
    exit 1
fi
grep -Fq 'Invalid PIN, or this is not a valid jdvrif "file-embedded" image' "$WORK/wrong_pin/recover.log"
if find "$WORK/wrong_pin" -maxdepth 1 -type f -name 'payload*.txt' -print -quit | grep -q .; then
    echo "wrong-PIN X-Twitter recovery wrote plaintext" >&2
    exit 1
fi
PASS=$((PASS + 1))

# A compressed payload beyond the content-dependent capacity must fail before
# encryption and print the measured carrier diagnostics.
mkdir "$WORK/capacity"
cp "$COVER" "$WORK/capacity/cover.jpg"
cp "$LARGE_PAYLOAD" "$WORK/capacity/large.bin"
if (cd "$WORK/capacity" && "$BIN" conceal -x cover.jpg large.bin > conceal.log 2>&1); then
    echo "oversized compressed X-Twitter payload was unexpectedly accepted" >&2
    exit 1
fi
CAPACITY_COVER_BYTES="$(stat -c '%s' "$WORK/capacity/cover.jpg")"
CAPACITY_COVER_KIB="$((CAPACITY_COVER_BYTES / 1024))"
grep -Fq "Cover Image: ${CAPACITY_COVER_KIB}KiB, 1280x960, Progressive YCbCr 4:2:0, Source-derived Q65 quantization (J-UNIWARD/STC)." "$WORK/capacity/conceal.log"
grep -Eq '^Compressed data file \(payload\) size: [0-9]+ bytes \([0-9]+KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Eq '^Theoretical J-UNIWARD/STC capacity limit for this cover image: +739 bytes \(~0KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Eq '^Conservative maximum compressed capacity with a 20-character filename: +625 bytes \(~0KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Eq '^Recommended  maximum compressed capacity with a 20-character filename: +0 bytes \(~0KiB\)\.$' "$WORK/capacity/conceal.log"
grep -Fq 'Data File Size Error:' "$WORK/capacity/conceal.log"
grep -Eq '^Already-compressed payload file size of [0-9]+ bytes \([0-9]+KiB\) exceeds the recommended maximum limit of 0 bytes \(~0KiB\) for this cover image\.$' "$WORK/capacity/conceal.log"
assert_no_output_image "$WORK/capacity"
PASS=$((PASS + 1))

# A second failure with a differently sized cover must report that source
# file's own binary-KiB size, rather than a cached or transcoded size.
mkdir "$WORK/capacity_alt_cover"
cp "$ALT_COVER" "$WORK/capacity_alt_cover/cover.jpg"
cp "$LARGE_PAYLOAD" "$WORK/capacity_alt_cover/large.bin"
if (cd "$WORK/capacity_alt_cover" && "$BIN" conceal -x cover.jpg large.bin > conceal.log 2>&1); then
    echo "oversized payload was unexpectedly accepted by alternate X-Twitter cover" >&2
    exit 1
fi
ALT_COVER_BYTES="$(stat -c '%s' "$WORK/capacity_alt_cover/cover.jpg")"
ALT_COVER_KIB="$((ALT_COVER_BYTES / 1024))"
if [[ "$ALT_COVER_KIB" == "$CAPACITY_COVER_KIB" ]]; then
    echo "X-Twitter cover-size fixtures do not exercise different displayed sizes" >&2
    exit 1
fi
grep -Fq "Cover Image: ${ALT_COVER_KIB}KiB, 1600x1200, Progressive YCbCr 4:2:0, Source-derived Q71 quantization (J-UNIWARD/STC)." "$WORK/capacity_alt_cover/conceal.log"
assert_no_output_image "$WORK/capacity_alt_cover"
PASS=$((PASS + 1))

# Cover and payload files each have an independent raw 5 MiB ceiling.
mkdir "$WORK/raw_payload"
cp "$COVER" "$WORK/raw_payload/cover.jpg"
cp "$PAYLOAD" "$WORK/raw_payload/payload.bin"
python3 - "$WORK/raw_payload/payload.bin" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.truncate(5 * 1024 * 1024 + 1)
PY
if (cd "$WORK/raw_payload" && "$BIN" conceal -x cover.jpg payload.bin > conceal.log 2>&1); then
    echo "over-5-MiB X-Twitter payload was unexpectedly accepted" >&2
    exit 1
fi
grep -Fq "Payload file exceeds X-Twitter's 5 MiB upload size limit" "$WORK/raw_payload/conceal.log"
assert_no_output_image "$WORK/raw_payload"
PASS=$((PASS + 1))

mkdir "$WORK/raw_cover"
cp "$COVER" "$WORK/raw_cover/cover.jpg"
cp "$PAYLOAD" "$WORK/raw_cover/payload.txt"
python3 - "$WORK/raw_cover/cover.jpg" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.truncate(5 * 1024 * 1024 + 1)
PY
if (cd "$WORK/raw_cover" && "$BIN" conceal -x cover.jpg payload.txt > conceal.log 2>&1); then
    echo "over-5-MiB X-Twitter cover was unexpectedly accepted" >&2
    exit 1
fi
grep -Fq "Cover image exceeds X-Twitter's 5 MiB upload size limit" "$WORK/raw_cover/conceal.log"
assert_no_output_image "$WORK/raw_cover"
PASS=$((PASS + 1))

# The 4096-pixel dimension check runs from the JPEG header before transcoding.
mkdir "$WORK/dimensions"
cp "$COVER" "$WORK/dimensions/cover.jpg"
cp "$PAYLOAD" "$WORK/dimensions/payload.txt"
python3 - "$WORK/dimensions/cover.jpg" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
pos = 2
while pos < len(data):
    if data[pos] != 0xFF:
        raise SystemExit("invalid JPEG while locating SOF")
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    marker = data[pos]
    pos += 1
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    length = int.from_bytes(data[pos:pos + 2], "big")
    if marker in range(0xC0, 0xD0) and marker not in (0xC4, 0xC8, 0xCC):
        data[pos + 5:pos + 7] = (4097).to_bytes(2, "big")
        path.write_bytes(data)
        break
    pos += length
else:
    raise SystemExit("SOF marker not found")
PY
if (cd "$WORK/dimensions" && "$BIN" conceal -x cover.jpg payload.txt > conceal.log 2>&1); then
    echo "over-4096-pixel X-Twitter cover was unexpectedly accepted" >&2
    exit 1
fi
grep -Fq "dimensions exceed X-Twitter's 4096x4096-pixel limit" "$WORK/dimensions/conceal.log"
assert_no_output_image "$WORK/dimensions"
PASS=$((PASS + 1))

echo "X-Twitter adaptive conceal tests passed: $PASS"
