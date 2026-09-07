#!/usr/bin/env python3

"""Check that the Python excerpts in the companion post still match the script.

`posting-via-the-bluesky-api.md` quotes `create_bsky_post.py` in several
places. Prose drifting away from code is the normal fate of a document like
that, and it is invisible until someone follows an excerpt that no longer
exists. This compares every ```python block against the script and reports
any line that is not in it.

Lines are compared after stripping indentation, so a block may quote a
function body without its enclosing `try:`/`def`. A line of `...` marks a
deliberate elision and is skipped. Every other line must appear verbatim.

    $ python3 verify_doc_excerpts.py          # exit 0 when in sync, 1 otherwise

No third-party dependencies: this is meant to run in CI or a git hook without
installing the posting script's requirements.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOC = HERE / "posting-via-the-bluesky-api.md"
SCRIPT = HERE / "create_bsky_post.py"
CODE_BLOCK_RE = re.compile(r"```python\n(.*?)```", re.DOTALL)
ELISION = "..."


def _excerpt_lines(block: str) -> list[str]:
    return [
        line.strip()
        for line in block.splitlines()
        if line.strip() and line.strip() != ELISION
    ]


def main() -> int:
    for path in (DOC, SCRIPT):
        if not path.is_file():
            print(f"missing file: {path}", file=sys.stderr)
            return 1

    script_lines = {line.strip() for line in SCRIPT.read_text().splitlines()}
    blocks = CODE_BLOCK_RE.findall(DOC.read_text())
    if not blocks:
        print(f"no ```python blocks found in {DOC.name}", file=sys.stderr)
        return 1

    drifted = 0
    for number, block in enumerate(blocks, start=1):
        lines = _excerpt_lines(block)
        missing = [line for line in lines if line not in script_lines]
        label = lines[0][:60] if lines else "(empty block)"
        if missing:
            drifted += 1
            print(f"[DRIFT] block {number}: {label}", file=sys.stderr)
            for line in missing:
                print(f"          not in {SCRIPT.name}: {line}", file=sys.stderr)
        else:
            print(f"[ok]    block {number}: {label}")

    if drifted:
        print(
            f"\n{drifted} of {len(blocks)} excerpts no longer match {SCRIPT.name}.",
            file=sys.stderr,
        )
        return 1
    print(f"\nAll {len(blocks)} excerpts match {SCRIPT.name}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
