#!/usr/bin/env python3

from __future__ import annotations

import base64
import importlib.util
import io
import pathlib
import unittest
from contextlib import redirect_stdout


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "src" / "dynqueue-parser.py"
SPEC = importlib.util.spec_from_file_location("dynqueue_parser", MODULE_PATH)
assert SPEC and SPEC.loader
PARSER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PARSER)


class ParserTests(unittest.TestCase):
    def test_real_chain(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("echo one && echo two"),
            ["echo one", "echo two"],
        )

    def test_double_quoted_andand_is_not_an_operator(self) -> None:
        self.assertIsNone(PARSER.split_top_level_andand('echo "one && two"'))

    def test_single_quoted_andand_is_not_an_operator(self) -> None:
        self.assertIsNone(PARSER.split_top_level_andand("echo 'one && two'"))

    def test_pipeline_stays_one_item(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("cat file | grep test && echo done"),
            ["cat file | grep test", "echo done"],
        )

    def test_redirection_stays_with_item(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("command > file && echo done"),
            ["command > file", "echo done"],
        )

    def test_test_expression_is_not_split(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand('[[ "$A" == one && "$B" == two ]] && echo done'),
            ['[[ "$A" == one && "$B" == two ]]', "echo done"],
        )

    def test_shell_control_construct_is_rejected(self) -> None:
        self.assertIsNone(PARSER.split_top_level_andand("if true; then echo one && echo two; fi"))

    def test_shell_state_mutation_is_rejected(self) -> None:
        for source in (
            "trap 'echo nope' INT && echo two",
            "set -e && echo two",
            "source ./setup.sh && echo two",
            "exec ./replacement && echo two",
            "NAME=value trap 'echo nope' INT && echo two",
        ):
            with self.subTest(source=source):
                self.assertIsNone(PARSER.split_top_level_andand(source))

    def test_unsafe_word_as_argument_is_not_rejected(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("echo trap && echo set"),
            ["echo trap", "echo set"],
        )

    def test_reserved_word_as_argument_does_not_reject_a_simple_chain(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("echo done && echo ready"),
            ["echo done", "echo ready"],
        )

    def test_nested_command_substitution_is_one_item(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("echo $(printf 'a && b') && echo done"),
            ["echo $(printf 'a && b')", "echo done"],
        )

    def test_brace_group_is_one_item(self) -> None:
        self.assertEqual(
            PARSER.split_top_level_andand("{ echo one && echo two; } && echo done"),
            ["{ echo one && echo two; }", "echo done"],
        )

    def test_cd_and_export_examples(self) -> None:
        self.assertEqual(PARSER.split_top_level_andand("cd /tmp && pwd"), ["cd /tmp", "pwd"])
        self.assertEqual(
            PARSER.split_top_level_andand('export TEST=hello && echo "$TEST"'),
            ["export TEST=hello", 'echo "$TEST"'],
        )

    def test_empty_or_incomplete_chain_is_rejected(self) -> None:
        self.assertIsNone(PARSER.split_top_level_andand("echo one &&"))
        self.assertIsNone(PARSER.split_top_level_andand("&& echo two"))

    def test_cli_outputs_base64_items(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            old_stdin = PARSER.sys.stdin
            try:
                PARSER.sys.stdin = io.StringIO("true && echo SHOULD_RUN")
                self.assertEqual(PARSER.main(), 0)
            finally:
                PARSER.sys.stdin = old_stdin

        decoded = [base64.b64decode(line).decode("utf-8") for line in output.getvalue().splitlines()]
        self.assertEqual(decoded, ["true", "echo SHOULD_RUN"])


if __name__ == "__main__":
    unittest.main()
