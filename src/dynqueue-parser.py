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
    at_word_start = True
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
            escaped = False
            at_word_start = False
            i += 1
            continue

        if char == "\\":
            escaped = True
            at_word_start = False
            i += 1
            continue

        if char in ("'", '"', "`"):
            quote = char
            at_word_start = False
            i += 1
            continue

        if char == "#" and at_word_start:
            comment = True
            i += 1
            continue

        if char == "(" :
            paren_depth += 1
            at_word_start = True
            i += 1
            continue

        if char == ")" and paren_depth:
            paren_depth -= 1
            at_word_start = True
            i += 1
            continue

        if char == "{":
            brace_depth += 1
            at_word_start = True
            i += 1
            continue

        if char == "}" and brace_depth:
            brace_depth -= 1
            at_word_start = True
            i += 1
            continue

        if char == "&" and i + 1 < len(source) and source[i + 1] == "&":
            if paren_depth == 0 and brace_depth == 0:
                item = source[start:i].strip()
                if not item:
                    return None
                parts.append(item)
                start = i + 2
                at_word_start = True
                i += 2
                continue

            at_word_start = True
            i += 2
            continue

        if char.isspace():
            at_word_start = True
        elif char in ";|&<>":
            at_word_start = True
        else:
            at_word_start = False
        i += 1

    if quote is not None or escaped or paren_depth != 0 or brace_depth != 0:
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
