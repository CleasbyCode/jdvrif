#!/bin/bash
# Create deterministic binary payloads and cover JPEGs for golden-file tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="$ROOT/testdata"
mkdir -p "$DATA/covers" "$DATA/payloads"

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

need_cmd ffmpeg
need_cmd python3
need_cmd zip

if [[ ! -f "$DATA/covers/cover_default.jpg" ]]; then
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i testsrc2=size=1280x960:rate=1 -frames:v 1 -q:v 10 \
        "$DATA/covers/cover_default.jpg"
fi

if [[ ! -f "$DATA/covers/cover_reddit.jpg" ]]; then
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i testsrc=size=1600x1200:rate=1 -frames:v 1 -q:v 8 \
        "$DATA/covers/cover_reddit.jpg"
fi

if [[ ! -f "$DATA/covers/cover_bluesky.jpg" ]]; then
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i smptebars=size=900x900:rate=1 -frames:v 1 -q:v 12 \
        "$DATA/covers/cover_bluesky.jpg"
fi

DATA="$DATA" python3 - <<'PY'
import os
import random
from pathlib import Path

root = Path(os.environ["DATA"]) / "payloads"
root.mkdir(parents=True, exist_ok=True)
specs = {
    "payload_multi.bin": (250_000, 42),
    "bsingle.bin": (62_000, 43),
    "bsplit.bin": (80_000, 44),
    "bxmp.bin": (150_000, 46),
}
for name, (size, seed) in specs.items():
    path = root / name
    if path.exists() and path.stat().st_size == size:
        continue
    rng = random.Random(seed)
    path.write_bytes(bytes(rng.randrange(256) for _ in range(size)))
PY

archive_src="$DATA/payloads/archive_src"
mkdir -p "$archive_src"
cp "$DATA/payloads/payload_text.txt" "$archive_src/readme.txt"
python3 - <<PY
import random
from pathlib import Path
path = Path("$archive_src/chunk.dat")
if not path.exists() or path.stat().st_size != 4096:
    rng = random.Random(45)
    path.write_bytes(bytes(rng.randrange(256) for _ in range(4096)))
PY

(
    cd "$archive_src"
    zip -q ../payload_archive.zip readme.txt chunk.dat
)

echo "Testdata ready under: $DATA"
