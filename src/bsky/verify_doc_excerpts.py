#!/usr/bin/env python3

"""Check that the Python excerpts in the companion post still match the script.

`posting-via-the-bluesky-api.md` quotes `create_bsky_post.py` in several
places. Each Python block must match a contiguous sequence of complete
statements in the script's syntax tree. Comments and formatting are ignored;
statement order, nesting, exception types, and literal values are checked.
A block can quote complete statements from inside a function, but excerpts
from separate locations must use separate blocks. Elisions are not expanded.
Every JSON block is also parsed so schematic examples remain valid JSON.

    $ python3 verify_doc_excerpts.py          # exit 0 when in sync, 1 otherwise

No third-party dependencies: this is meant to run in CI or a git hook without
installing the posting script's requirements.
"""

from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOC = HERE / "posting-via-the-bluesky-api.md"
SCRIPT = HERE / "create_bsky_post.py"
CODE_BLOCK_RE = re.compile(
    r"^```(python|json)[ \t]*\r?\n(.*?)^```[ \t]*\r?$",
    re.MULTILINE | re.DOTALL,
)


def _statement_sequences(tree: ast.AST) -> list[list[str]]:
    sequences = []
    for node in ast.walk(tree):
        for _, value in ast.iter_fields(node):
            if (
                isinstance(value, list)
                and value
                and all(isinstance(item, ast.stmt) for item in value)
            ):
                sequences.append([ast.dump(item) for item in value])
    return sequences


def check_document(document: str, source: str) -> list[str]:
    """Return actionable errors without importing or executing the helper."""
    try:
        sequences = _statement_sequences(ast.parse(source))
    except SyntaxError as exc:
        return [f"Invalid helper syntax at line {exc.lineno}: {exc.msg}"]

    problems = []
    python_blocks = 0
    for match in CODE_BLOCK_RE.finditer(document):
        language, block = match.groups()
        line = document.count("\n", 0, match.start()) + 1
        label = f"{language} block at document line {line}"
        if language == "json":
            try:
                json.loads(block)
            except json.JSONDecodeError as exc:
                problems.append(f"{label}: invalid JSON: {exc}")
            continue

        python_blocks += 1
        try:
            excerpt = ast.parse(block)
        except SyntaxError as exc:
            problems.append(f"{label}: invalid Python: {exc.msg}")
            continue
        expected = [ast.dump(statement) for statement in excerpt.body]
        if not expected:
            problems.append(f"{label}: empty Python excerpt")
            continue
        if not any(
            sequence[start:start + len(expected)] == expected
            for sequence in sequences
            for start in range(len(sequence) - len(expected) + 1)
        ):
            problems.append(
                f"{label}: no matching contiguous statement sequence in "
                f"{SCRIPT.name}; check the excerpt's order, nesting, and values"
            )
    if not python_blocks:
        problems.append("No Python excerpts found")
    return problems


def main() -> int:
    try:
        document = DOC.read_text(encoding="utf-8")
        source = SCRIPT.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        print(f"Could not read documentation or helper: {exc}", file=sys.stderr)
        return 1

    problems = check_document(document, source)
    if problems:
        for problem in problems:
            print(f"[FAIL] {problem}", file=sys.stderr)
        return 1
    languages = [match.group(1) for match in CODE_BLOCK_RE.finditer(document)]
    print(
        f"All {languages.count('python')} Python excerpts match {SCRIPT.name}; "
        f"all {languages.count('json')} JSON examples parse."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
