#!/bin/bash
# Local, network-free regression tests for the Bluesky posting helper.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HELPER="$ROOT/bsky/create_bsky_post.py"
PYTHON="${PYTHON:-python3}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Missing required command: $PYTHON" >&2
    exit 1
fi
if [[ ! -f "$HELPER" ]]; then
    echo "Missing Bluesky helper: $HELPER" >&2
    exit 1
fi

# The bsky/ folder is mirrored into the Rust port's tree, and the two copies
# must stay byte-identical: a fix applied to one and not the other is invisible
# until someone diffs them by hand. Checked here because this is the only suite
# that owns the helper. The mirror is optional -- a checkout carrying just the
# C++ tree still passes, it simply skips the comparison.
BSKY_DIR="$ROOT/bsky"
MIRROR_DIR="$ROOT/../rust_port/src/bsky"
BSKY_FILES=(
    create_bsky_post.py
    posting-via-the-bluesky-api.md
    README.md
    requirements.txt
    verify_doc_excerpts.py
)

for name in "${BSKY_FILES[@]}"; do
    if [[ ! -f "$BSKY_DIR/$name" ]]; then
        echo "Missing Bluesky helper file: $BSKY_DIR/$name" >&2
        exit 1
    fi
done

if [[ -d "$MIRROR_DIR" ]]; then
    mirror_drifted=0
    for name in "${BSKY_FILES[@]}"; do
        if [[ ! -f "$MIRROR_DIR/$name" ]]; then
            echo "Mirror is missing $name ($MIRROR_DIR/$name)" >&2
            mirror_drifted=1
        elif ! cmp -s "$BSKY_DIR/$name" "$MIRROR_DIR/$name"; then
            echo "Mirror differs from source: $name" >&2
            mirror_drifted=1
        fi
    done
    if [[ "$mirror_drifted" -ne 0 ]]; then
        echo "The two bsky/ copies must stay byte-identical; copy the newer one over." >&2
        exit 1
    fi
    echo "Bluesky helper mirror in sync (${#BSKY_FILES[@]} files)."
else
    echo "No Rust-port mirror at $MIRROR_DIR; skipping the parity check."
fi

# Python removes assert statements when optimization is enabled. Keep the
# helper's self-tests meaningful under both normal Python and python -O.
"$PYTHON" -B - "$HELPER" <<'PY'
import ast
import sys
from pathlib import Path

path = Path(sys.argv[1])
tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
assert_nodes = [node for node in ast.walk(tree) if isinstance(node, ast.Assert)]
if assert_nodes:
    lines = ", ".join(str(node.lineno) for node in assert_nodes[:10])
    raise SystemExit(f"optimization-unsafe assert statements remain at lines: {lines}")
PY

"$PYTHON" -B "$HELPER" --self-test
"$PYTHON" -B -O "$HELPER" --self-test

echo "Bluesky helper tests passed (normal and optimized Python)."
