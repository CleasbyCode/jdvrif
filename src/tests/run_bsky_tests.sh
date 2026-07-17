#!/bin/bash
# Local, network-free regression tests for the Bluesky posting helper.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HELPER="$ROOT/bsky/bsky_post.py"
PYTHON="${PYTHON:-python3}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Missing required command: $PYTHON" >&2
    exit 1
fi
if [[ ! -f "$HELPER" ]]; then
    echo "Missing Bluesky helper: $HELPER" >&2
    exit 1
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
