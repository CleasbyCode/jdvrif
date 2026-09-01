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

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" JPG image: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\..*/\1/p' "$1" | tail -n 1
}

assert_table_layout() {
    local image="$1"
    local expected="$2"
    local quant_reference="$3"
    local huffman_reference="$4"
    python3 - \
        "$image" \
        "$expected" \
        "$quant_reference" \
        "$huffman_reference" <<'PY'
import sys
from pathlib import Path

image_path = Path(sys.argv[1])
expected_name = sys.argv[2]
quant_reference_path = Path(sys.argv[3])
huffman_reference_path = Path(sys.argv[4])
layout_name = expected_name

expected_dqt_layouts = {
    "progressive_split": [
        (0x0043, [(0, 0)]),
        (0x0043, [(0, 1)]),
    ],
    "baseline_split": [
        (0x0043, [(0, 0)]),
        (0x0043, [(0, 1)]),
    ],
}

expected_dht_layouts = {
    "progressive_split": [
        (0x001B, [(0, 0, 8)]),
        (0x001B, [(0, 1, 8)]),
    ],
    "baseline_split": [
        (0x001F, [(0, 0, 12)]),
        (0x00B5, [(1, 0, 162)]),
        (0x001F, [(0, 1, 12)]),
        (0x00B5, [(1, 1, 162)]),
    ],
}

def parse_tables(path):
    data = path.read_bytes()
    if not data.startswith(b"\xff\xd8"):
        raise SystemExit(f"{path}: missing JPEG SOI")

    dqt_layout = []
    dqt_tables = []
    dht_layout = []
    dht_tables = []
    frame = None
    scan = None
    pos = 2
    while pos < len(data):
        if data[pos] != 0xFF:
            raise SystemExit(f"{path}: invalid marker at offset {pos}")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            raise SystemExit(f"{path}: truncated marker")

        marker = data[pos]
        pos += 1
        if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if pos + 2 > len(data):
            raise SystemExit(f"{path}: truncated segment length")

        length = int.from_bytes(data[pos:pos + 2], "big")
        end = pos + length
        if length < 2 or end > len(data):
            raise SystemExit(f"{path}: invalid segment length {length}")

        if marker == 0xDA:
            components = data[pos + 2]
            cursor = pos + 3
            selectors = []
            for _ in range(components):
                component_id = data[cursor]
                dc_ac = data[cursor + 1]
                selectors.append(
                    (component_id, dc_ac >> 4, dc_ac & 0x0F))
                cursor += 2
            if cursor + 3 != end:
                raise SystemExit(f"{path}: malformed SOS payload")
            scan = (
                selectors,
                data[cursor],
                data[cursor + 1],
                data[cursor + 2] >> 4,
                data[cursor + 2] & 0x0F,
            )
            break
        elif marker == 0xDB:
            definitions = []
            cursor = pos + 2
            while cursor < end:
                table_start = cursor
                pq_tq = data[cursor]
                cursor += 1
                precision = pq_tq >> 4
                table_id = pq_tq & 0x0F
                if precision not in (0, 1):
                    raise SystemExit(
                        f"{path}: invalid DQT precision {precision}")
                table_size = 64 if precision == 0 else 128
                if cursor + table_size > end:
                    raise SystemExit(f"{path}: truncated DQT table")
                cursor += table_size
                definitions.append((precision, table_id))
                dqt_tables.append(bytes(data[table_start:cursor]))
            if not definitions or cursor != end:
                raise SystemExit(f"{path}: malformed DQT payload")
            dqt_layout.append((length, definitions))
        elif marker == 0xC4:
            definitions = []
            cursor = pos + 2
            while cursor < end:
                table_start = cursor
                tc_th = data[cursor]
                cursor += 1
                table_class = tc_th >> 4
                table_id = tc_th & 0x0F
                if table_class not in (0, 1):
                    raise SystemExit(
                        f"{path}: invalid DHT table class {table_class}")
                if cursor + 16 > end:
                    raise SystemExit(f"{path}: truncated DHT code counts")
                symbol_count = sum(data[cursor:cursor + 16])
                cursor += 16
                if cursor + symbol_count > end:
                    raise SystemExit(f"{path}: truncated DHT symbols")
                cursor += symbol_count
                definitions.append(
                    (table_class, table_id, symbol_count))
                dht_tables.append(bytes(data[table_start:cursor]))
            if not definitions or cursor != end:
                raise SystemExit(f"{path}: malformed DHT payload")
            dht_layout.append((length, definitions))
        elif marker in {
            0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
            0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF,
        }:
            precision = data[pos + 2]
            height = int.from_bytes(data[pos + 3:pos + 5], "big")
            width = int.from_bytes(data[pos + 5:pos + 7], "big")
            component_count = data[pos + 7]
            cursor = pos + 8
            components = []
            for _ in range(component_count):
                component_id = data[cursor]
                sampling = data[cursor + 1]
                components.append((
                    component_id,
                    sampling >> 4,
                    sampling & 0x0F,
                    data[cursor + 2],
                ))
                cursor += 3
            if cursor != end:
                raise SystemExit(f"{path}: malformed SOF payload")
            frame = (
                marker, precision, width, height, components)

        pos = end
    else:
        raise SystemExit(f"{path}: JPEG SOS not found")

    if frame is None or scan is None:
        raise SystemExit(f"{path}: missing SOF or SOS")
    return dqt_layout, dqt_tables, dht_layout, dht_tables, frame, scan

dqt_layout, dqt_tables, dht_layout, dht_tables, frame, scan = \
    parse_tables(image_path)

expected_dqt = expected_dqt_layouts[layout_name]
if dqt_layout != expected_dqt:
    raise SystemExit(
        f"{image_path}: DQT layout {dqt_layout!r}, "
        f"expected {expected_dqt!r}")

_, quant_reference_tables, _, _, _, _ = \
    parse_tables(quant_reference_path)
if dqt_tables != quant_reference_tables:
    raise SystemExit(
        f"{image_path}: DQT table bytes differ from "
        f"{quant_reference_path}")

expected_dht = expected_dht_layouts[layout_name]
if dht_layout != expected_dht:
    raise SystemExit(
        f"{image_path}: DHT layout {dht_layout!r}, "
        f"expected {expected_dht!r}")

flattened_dht = b"".join(dht_tables)
if layout_name == "baseline_split":
    huffman_reference_path.write_bytes(flattened_dht)

expected_frame = {
    "progressive_split": (
        0xC2, 8, 1600, 1200,
        [(1, 1, 2, 0), (2, 1, 2, 1), (3, 1, 2, 1)],
    ),
    "baseline_split": (
        0xC0, 8, 1600, 1200,
        [(1, 1, 2, 0), (2, 1, 2, 1), (3, 1, 2, 1)],
    ),
}[expected_name]
if frame != expected_frame:
    raise SystemExit(
        f"{image_path}: frame {frame!r}, expected {expected_frame!r}")

if layout_name == "baseline_split":
    expected_scan = (
        [(1, 0, 0), (2, 1, 1), (3, 1, 1)],
        0, 63, 0, 0,
    )
    if scan != expected_scan:
        raise SystemExit(
            f"{image_path}: scan {scan!r}, expected {expected_scan!r}")
PY
}

PASS=0
FAIL=0

run_case() {
    local case_id="$1"
    local option="$2"
    local cover_rel="$3"
    local payload_rel="$4"
    local expected_table_layout="$5"

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

    if [[ "$expected_table_layout" != "." ]] &&
       ! assert_table_layout \
           "$embedded" \
           "$expected_table_layout" \
           "$cover" \
           "$TESTS/.work_roundtrip/split_dht_tables.bin"; then
        popd >/dev/null
        echo "[FAIL] $case_id: unexpected JPEG table-marker layout" >&2
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
    $'default\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_text.txt\t.'
    $'default_multiseg\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_multi.bin\t.'
    $'default_space_name\t.\ttestdata/covers/cover_default.jpg\t.work_roundtrip/input_payloads/payload space.txt\t.'
    $'default_zip\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_archive.zip\t.'
    $'bluesky\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsingle.bin\t.'
    $'bluesky_split\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsplit.bin\t.'
    $'bluesky_xmp\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bxmp.bin\t.'
    $'dqt_default\t.\t.work_roundtrip/input_covers/two_tables.jpg\ttestdata/payloads/payload_text.txt\tprogressive_split'
    $'dqt_bluesky\t-b\t.work_roundtrip/input_covers/two_tables.jpg\ttestdata/payloads/payload_text.txt\tbaseline_split'
)

mkdir -p "$TESTS/.work_roundtrip"
trap 'rm -rf "$TESTS/.work_roundtrip"' EXIT
mkdir -p "$TESTS/.work_roundtrip/input_payloads"
cp "$TESTS/testdata/payloads/payload_text.txt" \
    "$TESTS/.work_roundtrip/input_payloads/payload space.txt"
mkdir -p "$TESTS/.work_roundtrip/input_covers"
python3 - \
    "$TESTS/testdata/covers/cover_tables.jpg" \
    "$TESTS/.work_roundtrip/input_covers/two_tables.jpg" <<'PY'
import sys
from pathlib import Path

source_path = Path(sys.argv[1])
output_path = Path(sys.argv[2])
data = bytearray(source_path.read_bytes())
if not data.startswith(b"\xff\xd8"):
    raise SystemExit(f"{source_path}: missing JPEG SOI")

insert_at = None
duplicate_dqt = None
pos = 2
while pos < len(data):
    if data[pos] != 0xFF:
        raise SystemExit(f"{source_path}: invalid marker at offset {pos}")
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    if pos >= len(data):
        raise SystemExit(f"{source_path}: truncated marker")

    marker = data[pos]
    pos += 1
    if marker == 0xDA:
        break
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue
    if pos + 2 > len(data):
        raise SystemExit(f"{source_path}: truncated segment length")

    length = int.from_bytes(data[pos:pos + 2], "big")
    end = pos + length
    if length < 2 or end > len(data):
        raise SystemExit(f"{source_path}: invalid segment length {length}")

    if marker == 0xDB and length == 0x0043 and data[pos + 2] == 0x00:
        insert_at = end
        table_values = bytearray(data[pos + 3:end])
        table_values[0] = (
            table_values[0] + 1 if table_values[0] < 255 else 254
        )
        duplicate_dqt = (
            b"\xff\xdb\x00\x43\x01" +
            bytes(table_values)
        )
        break
    pos = end

if insert_at is None or duplicate_dqt is None:
    raise SystemExit(f"{source_path}: suitable table-0 DQT not found")

data[insert_at:insert_at] = duplicate_dqt

patched_selectors = 0
pos = 2
sof_markers = {
    0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
    0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF,
}
while pos < len(data):
    if data[pos] != 0xFF:
        raise SystemExit(f"{source_path}: invalid marker at offset {pos}")
    while pos < len(data) and data[pos] == 0xFF:
        pos += 1
    if pos >= len(data):
        raise SystemExit(f"{source_path}: truncated marker")

    marker = data[pos]
    pos += 1
    if marker == 0xDA:
        break
    if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        continue

    length = int.from_bytes(data[pos:pos + 2], "big")
    end = pos + length
    if length < 2 or end > len(data):
        raise SystemExit(f"{source_path}: invalid segment length {length}")

    if marker in sof_markers:
        payload = pos + 2
        components = data[payload + 5]
        if components < 2 or payload + 6 + components * 3 > end:
            raise SystemExit(f"{source_path}: malformed SOF")
        for component in range(1, components):
            data[payload + 6 + component * 3 + 2] = 1
            patched_selectors += 1
        break
    pos = end

if patched_selectors == 0:
    raise SystemExit(f"{source_path}: chroma quantization selectors not found")
output_path.write_bytes(data)
PY

for row in "${CASES[@]}"; do
    IFS=$'\t' read -r case_id option cover_rel payload_rel expected_table_layout <<<"$row"
    if [[ "$option" == "." ]]; then
        option=""
    fi
    if run_case "$case_id" "$option" "$cover_rel" "$payload_rel" "$expected_table_layout"; then
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
