#!/usr/bin/env python3
"""Small shell-aware lexer for DynQueue's Bash integration.

The parser deliberately has one job: find top-level ``&&`` operators.  It
does not attempt to execute or fully interpret shell code.  Quotes, escapes,
comments, command substitutions, subshells, and brace groups are kept inside
one queue item so that pipelines and shell state remain intact.
"""

from __future__ import annotations

import base64
import sys
from typing import Optional


def split_top_level_andand(source: str) -> Optional[list[str]]:
    """Return queue items for a top-level ``&&`` chain, or ``None``.

    This is intentionally a lexical check rather than a naïve ``split``.
    Bash itself remains responsible for parsing and executing each returned
    item.  A malformed or incomplete chain is rejected instead of being
    altered by the integration.
    """

    if not source or "&&" not in source:
        return None

    parts: list[str] = []
    start = 0
    quote: Optional[str] = None
    escaped = False
    comment = False
    paren_depth = 0
    brace_depth = 0
    test_depth = 0
    at_word_start = True
    command_position = True
    word: list[str] = []
    word_plain = True
    word_active = False
    contains_shell_construct = False
    contains_unsafe_shell_command = False
    shell_construct_words = {
        "case",
        "coproc",
        "do",
        "done",
        "elif",
        "else",
        "esac",
        "fi",
        "for",
        "function",
        "if",
        "in",
        "select",
        "then",
        "time",
        "until",
        "while",
    }
    # These commands can replace DynQueue's DEBUG hook, terminate/replace the
    # interactive shell, or mutate the shell machinery that identifies the
    # current history entry.  Let Bash execute such lines normally rather
    # than trying to queue them and risk replaying the original compound line.
    unsafe_shell_words = {
        ".",
        "bind",
        "builtin",
        "enable",
        "eval",
        "exec",
        "exit",
        "fc",
        "history",
        "logout",
        "return",
        "set",
        "shopt",
        "source",
        "trap",
        "typeset",
        "unset",
    }

    def is_assignment_word(value: str) -> bool:
        name, separator, _ = value.partition("=")
        return bool(separator and name and (name[0].isalpha() or name[0] == "_") and all(character.isalnum() or character == "_" for character in name[1:]))

    def finish_word() -> None:
        nonlocal word_active, word_plain, contains_shell_construct, contains_unsafe_shell_command, command_position
        word_value = "".join(word)
        if command_position and word_active and word_plain:
            if word_value in shell_construct_words:
                contains_shell_construct = True
            if word_value in unsafe_shell_words:
                contains_unsafe_shell_command = True
        if word_active:
            # Assignment prefixes do not end Bash's command position.  This
            # keeps `NAME=value trap ...` in the conservative fallback path.
            command_position = is_assignment_word(word_value)
        word.clear()
        word_active = False
        word_plain = True

    i = 0

    while i < len(source):
        char = source[i]

        if comment:
            if char in "\r\n":
                comment = False
                at_word_start = True
            i += 1
            continue

        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\" and quote in ('"', "`"):
                escaped = True
            elif char == quote:
                quote = None
            i += 1
            continue

        if escaped:
            word_active = True
            word_plain = False
            word.append(char)
            escaped = False
            at_word_start = False
            i += 1
            continue

        if char == "\\":
            word_active = True
            word_plain = False
            escaped = True
            at_word_start = False
            i += 1
            continue

        if char in ("'", '"', "`"):
            word_active = True
            word_plain = False
            quote = char
            at_word_start = False
            i += 1
            continue

        if char == "#" and at_word_start:
            comment = True
            i += 1
            continue

        if at_word_start and char == "[" and i + 1 < len(source) and source[i + 1] == "[":
            finish_word()
            test_depth += 1
            command_position = False
            at_word_start = True
            i += 2
            continue

        if at_word_start and char == "]" and i + 1 < len(source) and source[i + 1] == "]":
            finish_word()
            if test_depth:
                test_depth -= 1
            else:
                return None
            at_word_start = True
            command_position = False
            i += 2
            continue

        if char == "(" :
            finish_word()
            paren_depth += 1
            at_word_start = True
            command_position = True
            i += 1
            continue

        if char == ")" and paren_depth:
            finish_word()
            paren_depth -= 1
            at_word_start = True
            command_position = False
            i += 1
            continue

        if char == "{":
            finish_word()
            brace_depth += 1
            at_word_start = True
            command_position = True
            i += 1
            continue

        if char == "}" and brace_depth:
            finish_word()
            brace_depth -= 1
            at_word_start = True
            command_position = False
            i += 1
            continue

        if char == "&" and i + 1 < len(source) and source[i + 1] == "&":
            finish_word()
            if paren_depth == 0 and brace_depth == 0 and test_depth == 0:
                item = source[start:i].strip()
                if not item:
                    return None
                parts.append(item)
                start = i + 2
                at_word_start = True
                command_position = True
                i += 2
                continue

            at_word_start = True
            command_position = True
            i += 2
            continue

        if char.isspace():
            finish_word()
            at_word_start = True
        elif char in ";|&<>":
            finish_word()
            at_word_start = True
            command_position = True
        else:
            word_active = True
            word.append(char)
            at_word_start = False
        i += 1

    finish_word()

    if (
        quote is not None
        or escaped
        or paren_depth != 0
        or brace_depth != 0
        or test_depth != 0
        or contains_shell_construct
        or contains_unsafe_shell_command
    ):
        return None

    tail = source[start:].strip()
    if not tail:
        return None
    parts.append(tail)

    return parts if len(parts) >= 2 else None


def main() -> int:
    source = sys.stdin.read()
    # The Bash hook uses printf without a trailing newline.  Removing only
    # line-ending characters keeps command whitespace otherwise untouched.
    source = source.rstrip("\r\n")
    parts = split_top_level_andand(source)
    if not parts:
        return 0

    for part in parts:
        encoded = base64.b64encode(part.encode("utf-8")).decode("ascii")
        print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
