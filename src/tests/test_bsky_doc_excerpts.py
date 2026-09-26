#!/usr/bin/env python3
"""Regression tests for documentation drift that line-membership checks miss."""

import importlib.util
import textwrap
import unittest
from pathlib import Path

CHECKER_PATH = Path(__file__).resolve().parents[1] / "bsky/verify_doc_excerpts.py"
SPEC = importlib.util.spec_from_file_location("bsky_doc_checker", CHECKER_PATH)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


def fence(code: str, language: str = "python") -> str:
    return f"```{language}\n{textwrap.dedent(code).strip()}\n```\n"


class DocExcerptTests(unittest.TestCase):
    def test_complete_statements_allow_comments_and_formatting(self):
        source = "def example():\n    # explanation\n    value = 'hello'\n    return value\n"
        document = fence('value="hello"\nreturn value')
        self.assertEqual(CHECKER.check_document(document, source), [])

    def test_stale_handler_fails_even_when_old_lines_exist_elsewhere(self):
        source = textwrap.dedent('''\
            def inspect_image():
                try:
                    decode()
                except (OSError, ValueError) as exc:
                    raise ValueError("invalid") from exc

            def unrelated():
                try:
                    decode()
                except ValueError as exc:
                    raise ValueError("invalid") from exc
        ''')
        excerpt = source[:source.index("\ndef unrelated")].replace(
            "(OSError, ValueError)", "ValueError"
        )
        # The old checker would accept every line of this stale function.
        source_lines = {line.strip() for line in source.splitlines()}
        self.assertTrue(all(line.strip() in source_lines for line in excerpt.splitlines()))
        self.assertTrue(CHECKER.check_document(fence(excerpt), source))

    def test_reordered_or_noncontiguous_statements_fail(self):
        source = "first = 1\nmiddle = 2\nlast = 3\n"
        for excerpt in ("middle = 2\nfirst = 1", "first = 1\nlast = 3"):
            with self.subTest(excerpt=excerpt):
                self.assertTrue(CHECKER.check_document(fence(excerpt), source))

    def test_nesting_change_fails(self):
        source = "if enabled:\n    first()\n    second()\n"
        excerpt = "if enabled:\n    first()\nsecond()\n"
        self.assertTrue(CHECKER.check_document(fence(excerpt), source))

    def test_changed_literal_and_unmarked_elision_fail(self):
        source = "def example():\n    value = 300\n    return value\n"
        for excerpt in (
            source.replace("300", "301"),
            source.replace("    value = 300", "    ..."),
        ):
            with self.subTest(excerpt=excerpt):
                self.assertTrue(CHECKER.check_document(fence(excerpt), source))

    def test_invalid_python_empty_excerpt_and_missing_excerpts_fail(self):
        for document in (fence("if:"), fence("# comment only"), "No excerpts"):
            with self.subTest(document=document):
                self.assertTrue(CHECKER.check_document(document, "value = 1"))

    def test_json_examples_must_parse(self):
        source = "value = 1"
        document = fence(source)
        self.assertEqual(
            CHECKER.check_document(document + fence('{"images": []}', "json"), source),
            [],
        )
        for invalid in ('{"images": [ … ]}', '{"images": [],}'):
            with self.subTest(invalid=invalid):
                errors = CHECKER.check_document(document + fence(invalid, "json"), source)
                self.assertTrue(any("invalid JSON" in error for error in errors))

    def test_crlf_fences_and_separate_locations(self):
        source = "first = 1\nmiddle = 2\nlast = 3\n"
        document = (fence("first = 1") + fence("last = 3")).replace("\n", "\r\n")
        self.assertEqual(CHECKER.check_document(document, source), [])


if __name__ == "__main__":
    unittest.main()
