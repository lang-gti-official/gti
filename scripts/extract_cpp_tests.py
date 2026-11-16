#!/usr/bin/env python3
"""Extract gti-test candidates from C++ frontend tests with inline GTI sources.

Temporary tooling for phase 4 of the test-suite redesign
(docs/plans/test-suite-redesign.md, section 11.1); deleted with the old suite.

A C++ test file is tokenized with a real C++ lexer (comments, preprocessor
lines, ordinary/raw/prefixed string literals with escapes decoded, adjacent
literal concatenation, character literals and digit separators), split into
functions and statements, and searched for calls of the file's analysis
helpers. Each helper call whose source argument is a compile-time string --
literals, `+` of literals and named string constants, constant-returning
helper functions, or one element of a range-for over a braced or named list of
such strings -- becomes a case. Anything else (strings assembled at run time,
loops over computed values, extra source files) is flagged, never guessed.

The expectation of a case comes from the helper's declared kind (accept,
reject with a code argument) or, for helpers that return the analysis result,
from the assertions that later statements make on the result variable.
Assertions the format cannot express (MIR/HIR structure, message text, spans,
fix-its, rendered output) are recorded per case as dropped, for review.

`extract` writes the candidates, runs `gti-test --bless` once to place the
`//~` annotations on the lines the compiler reports, and accepts a case only
when the compiler's error-code set equals the C++ test's asserted set (or the
C++ test asserted acceptance and the compiler accepts). Everything else stays
in the staging directory with a reason. `survey` classifies test files by
what their helpers do.

usage:
  extract_cpp_tests.py survey [FILES...]
  extract_cpp_tests.py cases FILE                 # print parsed cases
  extract_cpp_tests.py extract FILE --gti-test BIN [--staging DIR] [--apply]
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
CODE = re.compile(r"^GTI-[A-Z]+[0-9]+$")

# --------------------------------------------------------------------------
# Lexer


@dataclasses.dataclass
class Token:
    kind: str  # ident, number, string, char, punct
    text: str
    line: int
    value: str = ""  # decoded value of a string literal
    raw: bool = False
    end_line: int = 0


_PUNCT3 = ("<<=", ">>=", "...", "->*")
_PUNCT2 = ("::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
           "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", ".*")
_SIMPLE_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\",
                   "'": "'", '"': '"', "?": "?", "a": "\a", "b": "\b",
                   "f": "\f", "v": "\v"}


class LexError(Exception):
    pass


def _decode(body: str, line: int) -> str:
    out = []
    index = 0
    while index < len(body):
        char = body[index]
        if char != "\\":
            out.append(char)
            index += 1
            continue
        index += 1
        if index >= len(body):
            raise LexError(f"line {line}: dangling escape")
        escape = body[index]
        if escape == "x":
            match = re.match(r"[0-9A-Fa-f]+", body[index + 1:])
            if not match:
                raise LexError(f"line {line}: bad \\x escape")
            out.append(chr(int(match.group(0), 16)))
            index += 1 + len(match.group(0))
        elif escape in "01234567":
            match = re.match(r"[0-7]{1,3}", body[index:])
            out.append(chr(int(match.group(0), 8)))
            index += len(match.group(0))
        elif escape in "uU":
            width = 4 if escape == "u" else 8
            out.append(chr(int(body[index + 1:index + 1 + width], 16)))
            index += 1 + width
        elif escape == "\n":  # line continuation inside a literal
            index += 1
        elif escape in _SIMPLE_ESCAPES:
            out.append(_SIMPLE_ESCAPES[escape])
            index += 1
        else:
            raise LexError(f"line {line}: unknown escape \\{escape}")
    return "".join(out)


def tokenize(text: str) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    line = 1
    length = len(text)
    at_line_start = True
    while index < length:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
            at_line_start = True
            continue
        if char in " \t\r\f\v":
            index += 1
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise LexError(f"line {line}: unterminated comment")
            line += text.count("\n", index, end)
            index = end + 2
            continue
        if char == "#" and at_line_start:
            # A preprocessor line, with backslash continuations.
            while index < length:
                end = text.find("\n", index)
                if end < 0:
                    index = length
                    break
                if text[end - 1] == "\\":
                    line += 1
                    index = end + 1
                    continue
                index = end
                break
            continue
        at_line_start = False
        prefix = re.match(r'(u8|u|U|L)?R"', text[index:index + 4])
        if prefix and not (index and (text[index - 1].isalnum() or
                                      text[index - 1] == "_")):
            start = index + len(prefix.group(0))
            open_paren = text.find("(", start)
            delimiter = text[start:open_paren]
            if open_paren < 0 or len(delimiter) > 16 or re.search(
                    r'[\s()\\]', delimiter):
                raise LexError(f"line {line}: bad raw string delimiter")
            closing = ")" + delimiter + '"'
            end = text.find(closing, open_paren + 1)
            if end < 0:
                raise LexError(f"line {line}: unterminated raw string")
            value = text[open_paren + 1:end]
            token = Token("string", text[index:end + len(closing)], line,
                          value, True)
            line += text.count("\n", index, end + len(closing))
            token.end_line = line
            tokens.append(token)
            index = end + len(closing)
            continue
        prefix = re.match(r"(u8|u|U|L)?(\"|')", text[index:index + 3])
        if prefix and not (index and (text[index - 1].isalnum() or
                                      text[index - 1] == "_")):
            quote = prefix.group(2)
            cursor = index + len(prefix.group(0))
            while cursor < length and text[cursor] != quote:
                if text[cursor] == "\\":
                    cursor += 1
                elif text[cursor] == "\n":
                    raise LexError(f"line {line}: newline in literal")
                cursor += 1
            if cursor >= length:
                raise LexError(f"line {line}: unterminated literal")
            body = text[index + len(prefix.group(0)):cursor]
            kind = "string" if quote == '"' else "char"
            tokens.append(Token(kind, text[index:cursor + 1], line,
                                _decode(body, line), False, line))
            index = cursor + 1
            continue
        if char.isalpha() or char == "_":
            match = re.match(r"[A-Za-z_][A-Za-z_0-9]*", text[index:])
            tokens.append(Token("ident", match.group(0), line))
            index += len(match.group(0))
            continue
        if char.isdigit() or (char == "." and index + 1 < length and
                              text[index + 1].isdigit()):
            cursor = index + 1
            while cursor < length:
                current = text[cursor]
                if current in "eEpP" and cursor + 1 < length and \
                        text[cursor + 1] in "+-":
                    cursor += 2
                elif current.isalnum() or current in "._":
                    cursor += 1
                elif current == "'" and cursor + 1 < length and \
                        text[cursor + 1].isalnum():
                    cursor += 2  # digit separator
                else:
                    break
            tokens.append(Token("number", text[index:cursor], line))
            index = cursor
            continue
        for operator in _PUNCT3 + _PUNCT2:
            if text.startswith(operator, index):
                tokens.append(Token("punct", operator, line))
                index += len(operator)
                break
        else:
            tokens.append(Token("punct", char, line))
            index += 1
    return _merge_adjacent_strings(tokens)


def _merge_adjacent_strings(tokens: list[Token]) -> list[Token]:
    merged: list[Token] = []
    for token in tokens:
        if token.kind == "string" and merged and merged[-1].kind == "string":
            previous = merged[-1]
            previous.value += token.value
            previous.text += " " + token.text
            previous.raw = previous.raw or token.raw
            previous.end_line = token.end_line
            continue
        if (token.kind == "ident" and token.text in ("s", "sv") and merged and
                merged[-1].kind == "string" and
                merged[-1].text.endswith('"')):
            # A std::string / string_view literal suffix: "text"s. The lexer
            # dropped no space only when they were adjacent; accept either.
            continue
        merged.append(token)
    return merged


# --------------------------------------------------------------------------
# Structure: functions, statements, constants

_OPEN = {"(": ")", "[": "]", "{": "}"}


def match_close(tokens: list[Token], index: int) -> int:
    """Index of the bracket closing tokens[index]."""
    opener = tokens[index].text
    closer = _OPEN[opener]
    depth = 0
    for cursor in range(index, len(tokens)):
        text = tokens[cursor].text
        if tokens[cursor].kind != "punct":
            continue
        if text in _OPEN:
            depth += 1
        elif text in (")", "]", "}"):
            depth -= 1
            if depth == 0:
                if text != closer:
                    raise LexError(
                        f"line {tokens[cursor].line}: mismatched {text}")
                return cursor
    raise LexError(f"line {tokens[index].line}: unclosed {opener}")


@dataclasses.dataclass
class Function:
    name: str
    params: list[Token]
    body: list[Token]
    line: int
    start_line: int = 0
    end_line: int = 0


@dataclasses.dataclass
class Unit:
    path: Path
    tokens: list[Token]
    functions: dict[str, Function]
    globals: list[list[Token]]  # namespace-scope declarations


_FUNCTION_TAIL = {"const", "noexcept", "override", "final", "mutable"}


def _function_head(tokens: list[Token], brace: int) -> Optional[tuple[str, int]]:
    cursor = brace - 1
    # Skip a trailing return type `-> T`.
    scan = cursor
    while scan >= 0 and tokens[scan].text not in (")", ";", "}", "{", "->"):
        scan -= 1
    if scan >= 0 and tokens[scan].text == "->":
        cursor = scan - 1
    while cursor >= 0 and tokens[cursor].text in _FUNCTION_TAIL:
        cursor -= 1
    if cursor < 0 or tokens[cursor].text != ")":
        return None
    depth = 0
    for open_index in range(cursor, -1, -1):
        text = tokens[open_index].text
        if text == ")":
            depth += 1
        elif text == "(":
            depth -= 1
            if depth == 0:
                break
    else:
        return None
    name_index = open_index - 1
    if name_index < 0 or tokens[name_index].kind != "ident":
        return None
    if tokens[name_index].text in ("if", "for", "while", "switch", "catch"):
        return None
    return tokens[name_index].text, open_index


def parse_unit(path: Path, text: Optional[str] = None) -> Unit:
    tokens = tokenize(text if text is not None else path.read_text())
    functions: dict[str, Function] = {}
    globals_: list[list[Token]] = []
    index = 0
    statement_start = 0
    while index < len(tokens):
        token = tokens[index]
        if token.text == "{" and token.kind == "punct":
            if index >= 1 and (tokens[index - 1].text == "namespace" or (
                    index >= 2 and tokens[index - 2].text == "namespace")) or (
                    index >= 2 and tokens[index - 2].text == "extern"):
                index += 1
                statement_start = index
                continue
            close = match_close(tokens, index)
            head = _function_head(tokens, index)
            is_init = index >= 1 and tokens[index - 1].text in ("=", ",", "(")
            if head and not is_init:
                name, open_index = head
                params = tokens[open_index + 1:match_close(tokens, open_index)]
                functions.setdefault(name, Function(
                    name, params, tokens[index + 1:close], token.line,
                    tokens[statement_start].line, tokens[close].line))
                index = close + 1
                statement_start = index
                continue
            index = close + 1
            continue
        if token.text == "}" and token.kind == "punct":
            index += 1  # end of a namespace
            statement_start = index
            continue
        if token.text == ";" and token.kind == "punct":
            globals_.append(tokens[statement_start:index])
            statement_start = index + 1
        index += 1
    return Unit(path, tokens, functions, globals_)


@dataclasses.dataclass
class Stmt:
    kind: str  # expr, block, if, for, while, do, switch, try, return
    tokens: list[Token]  # the expression / condition / for header
    children: list["Stmt"]
    line: int


def parse_block(tokens: list[Token]) -> list[Stmt]:
    statements = []
    index = 0
    while index < len(tokens):
        statement, index = _parse_statement(tokens, index)
        if statement:
            statements.append(statement)
    return statements


def _parse_statement(tokens: list[Token], index: int):
    token = tokens[index]
    if token.text == ";":
        return None, index + 1
    if token.text == "{" and token.kind == "punct":
        close = match_close(tokens, index)
        return Stmt("block", [], parse_block(tokens[index + 1:close]),
                    token.line), close + 1
    if token.kind == "ident" and token.text in ("if", "while", "switch",
                                                "for"):
        cursor = index + 1
        if token.text == "if" and tokens[cursor].text == "constexpr":
            cursor += 1
        close = match_close(tokens, cursor)
        header = tokens[cursor + 1:close]
        body, after = _parse_statement(tokens, close + 1)
        children = [body] if body else []
        kind = token.text
        if kind == "if" and after < len(tokens) and tokens[after].text == "else":
            other, after = _parse_statement(tokens, after + 1)
            if other:
                other.kind = other.kind if other.kind != "block" else "block"
                children.append(Stmt("else", [], [other], other.line))
        return Stmt(kind, header, children, token.line), after
    if token.kind == "ident" and token.text == "do":
        body, after = _parse_statement(tokens, index + 1)
        # while ( ... ) ;
        close = match_close(tokens, after + 1)
        return Stmt("do", tokens[after + 2:close], [body] if body else [],
                    token.line), close + 2
    if token.kind == "ident" and token.text == "try":
        body, after = _parse_statement(tokens, index + 1)
        children = [body] if body else []
        while after < len(tokens) and tokens[after].text == "catch":
            close = match_close(tokens, after + 1)
            handler, after = _parse_statement(tokens, close + 1)
            if handler:
                children.append(handler)
        return Stmt("try", [], children, token.line), after
    # An expression or declaration statement, up to `;` at depth zero.
    cursor = index
    while cursor < len(tokens):
        text = tokens[cursor].text
        if tokens[cursor].kind == "punct" and text in _OPEN:
            cursor = match_close(tokens, cursor) + 1
            continue
        if tokens[cursor].kind == "punct" and text == ";":
            break
        cursor += 1
    kind = "return" if token.text == "return" else "expr"
    return Stmt(kind, tokens[index:cursor], [], token.line), cursor + 1


def split_list(tokens: list[Token]) -> list[list[Token]]:
    """Elements of a braced initializer list (a trailing comma is legal)."""
    parts = split_top(tokens, ",")
    return parts[:-1] if parts and not parts[-1] else parts


def split_top(tokens: list[Token], separator: str) -> list[list[Token]]:
    parts: list[list[Token]] = [[]]
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.kind == "punct" and token.text in _OPEN:
            close = match_close(tokens, index)
            parts[-1].extend(tokens[index:close + 1])
            index = close + 1
            continue
        if token.kind == "punct" and token.text == separator:
            parts.append([])
        else:
            parts[-1].append(token)
        index += 1
    return parts if parts != [[]] else []


def compact(tokens: list[Token], var: str = "") -> str:
    """Tokens as compact text; identifier `var` (never text inside a
    literal) is written `$`."""
    out = []
    previous = None
    for token in tokens:
        if var and token.kind == "ident" and token.text == var:
            if previous is not None and previous.kind in ("ident", "number"):
                out.append(" ")
            out.append("$")
            previous = token
            continue
        if previous is not None and previous.kind in ("ident", "number") and \
                token.kind in ("ident", "number"):
            out.append(" ")
        if token.kind == "string":
            out.append(json.dumps(token.value if len(token.value) < 400 else
                                  token.value[:397] + "..."))
        else:
            out.append(token.text)
        previous = token
    return "".join(out)


# String-valued compile-time expressions -----------------------------------

_STRING_TYPES = ("string", "string_view", "char")
FORMATTED = "\0formatted\0"


class Env:
    def __init__(self, parent: Optional["Env"] = None):
        self.parent = parent
        self.strings: dict[str, Optional[str]] = {}
        self.lists: dict[str, Optional[list[str]]] = {}

    def lookup(self, name: str):
        env = self
        while env:
            if name in env.strings:
                return ("string", env.strings[name])
            if name in env.lists:
                return ("list", env.lists[name])
            env = env.parent
        return None


class Evaluator:
    def __init__(self, unit: Unit, extra_units: list[Unit]):
        self.functions: dict[str, Function] = {}
        for other in extra_units + [unit]:
            self.functions.update(other.functions)
        self.unit = unit
        self.global_env = Env()
        mutated = self._mutated_names(unit.tokens)
        for other in extra_units + [unit]:
            for declaration in other.globals:
                self.declare(declaration, self.global_env, mutated)

    @staticmethod
    def _mutated_names(tokens: list[Token]) -> set[str]:
        names = set()
        for index, token in enumerate(tokens[:-1]):
            following = tokens[index + 1].text
            if token.kind == "ident" and following in ("+=",):
                names.add(token.text)
            if token.kind == "ident" and following == "." and index + 2 < len(
                    tokens) and tokens[index + 2].text in (
                        "append", "push_back", "insert", "replace", "erase",
                        "emplace_back", "clear", "assign"):
                names.add(token.text)
        return names

    def declare(self, tokens: list[Token], env: Env,
                mutated: set[str]) -> Optional[str]:
        """Records `T name = expr` when T is a string or list of strings."""
        if not tokens:
            return None
        texts = [t.text for t in tokens]
        # name followed by `=`, `{` or `(` after a type.
        for index in range(1, len(tokens)):
            token = tokens[index]
            if token.kind != "ident":
                continue
            following = tokens[index + 1].text if index + 1 < len(tokens) else ""
            array = following == "[" and index + 3 < len(tokens) and \
                tokens[index + 2].text == "]"
            if following not in ("=", "{") and not array:
                continue
            type_tokens = texts[:index]
            if not type_tokens or type_tokens[-1] in ("=", ".", "->", "::",
                                                      "(", ","):
                continue
            if any(word in ("return",) for word in type_tokens):
                return None
            init_start = index + (4 if array else 1)
            if array and init_start < len(tokens) and \
                    tokens[init_start - 1].text == "=":
                pass
            if following == "=":
                init = tokens[index + 2:]
            elif array:
                init = tokens[index + 4:] if tokens[index + 3].text == "=" \
                    else tokens[index + 3:]
            else:
                init = tokens[index + 1:]
            name = token.text
            joined = " ".join(type_tokens)
            is_list = array or re.search(r"\b(vector|array|initializer_list)\b",
                                         joined)
            is_string = any(word in type_tokens for word in _STRING_TYPES) or \
                type_tokens[-1] in ("auto",)
            if not (is_list or is_string):
                return None
            if name in mutated:
                if is_list:
                    env.lists[name] = None
                else:
                    env.strings[name] = None
                return name
            if is_list:
                items = None
                if init and init[0].text == "{":
                    close = match_close(init, 0)
                    items = []
                    for part in split_list(init[1:close]):
                        value = self.evaluate(part, env)
                        if value is None:
                            items = None
                            break
                        items.append(value)
                env.lists[name] = items
            else:
                if init and init[0].text == "{" and following == "{":
                    close = match_close(init, 0)
                    init = init[1:close]
                value = self.evaluate(init, env)
                if value is None and type_tokens[-1] == "auto":
                    return None
                env.strings[name] = value
            return name
        return None

    def evaluate(self, tokens: list[Token], env: Env,
                 depth: int = 0) -> Optional[str]:
        if not tokens or depth > 20:
            return None
        parts = split_top(tokens, "+")
        if len(parts) > 1:
            values = [self.evaluate(part, env, depth + 1) for part in parts]
            return None if any(v is None for v in values) else "".join(values)
        tokens = parts[0] if parts else tokens
        if len(tokens) == 1:
            token = tokens[0]
            if token.kind == "string":
                return token.value
            if token.kind == "ident":
                found = env.lookup(token.text)
                if found and found[0] == "string":
                    return found[1]
            return None
        if tokens[0].text == "(" and match_close(tokens, 0) == len(tokens) - 1:
            return self.evaluate(tokens[1:-1], env, depth + 1)
        texts = [t.text for t in tokens]
        # std::string(expr), std::string{expr}, std::string_view(expr)
        if len(texts) >= 4 and texts[0] == "std" and texts[1] == "::" and \
                texts[2] in ("string", "string_view") and texts[3] in "({" and \
                match_close(tokens, 3) == len(tokens) - 1:
            return self.evaluate(tokens[4:-1], env, depth + 1)
        # Formatter output: `formatter.format(E)` or
        # `lang::Formatter().format(E)`; marked so the case can carry
        # `//@ format-stable` instead of a formatted copy.
        if len(texts) >= 4 and texts[-1] == ")" and "format" in texts:
            at = len(texts) - 1 - texts[::-1].index("format")
            if at + 1 < len(tokens) and texts[at + 1] == "(" and \
                    match_close(tokens, at + 1) == len(tokens) - 1 and \
                    at >= 1 and texts[at - 1] == "." and \
                    ("Formatter" in texts[:at] or len(texts[:at]) == 2):
                inner = self.evaluate(tokens[at + 2:-1], env, depth + 1)
                return None if inner is None else FORMATTED + inner
        # A call of a parameterless function returning a constant string.
        if len(tokens) == 3 and tokens[0].kind == "ident" and \
                texts[1:] == ["(", ")"]:
            function = self.functions.get(texts[0])
            if function and not function.params:
                body = parse_block(function.body)
                if len(body) == 1 and body[0].kind == "return":
                    return self.evaluate(body[0].tokens[1:], self.global_env,
                                         depth + 1)
        return None


# --------------------------------------------------------------------------
# Helper specifications


@dataclasses.dataclass
class Helper:
    """What one analysis helper of a test file does.

    kind: `analyze` returns the analysis result, whose later assertions are
    read from the call site; `accept` asserts acceptance; `reject` asserts
    rejection with the code at `code_arg` (or `default_code`).
    """

    kind: str
    source_arg: int = 0
    code_arg: Optional[int] = None
    default_code: Optional[str] = None
    prefix: str = ""
    suffix: str = ""
    method: bool = False  # called as `.name(` (lang::Frontend().analyze)
    name_arg: Optional[int] = None
    # Assertions the helper makes that a .gti test does not carry.
    dropped: tuple[str, ...] = ()
    # How the helper's analysis differs from gti-test's (checked by the
    # compiler-run comparison; listed for review).
    differences: tuple[str, ...] = ()


@dataclasses.dataclass
class FileSpec:
    area: str
    helpers: dict[str, Helper] = dataclasses.field(default_factory=dict)
    # Subject of the file's tests, for the first-line comments.
    topic: str = ""
    # Named test functions to convert; empty means every function.
    only: tuple[str, ...] = ()
    # Per named test function, the area it lands in instead of `area`.
    areas: dict[str, str] = dataclasses.field(default_factory=dict)
    skip: tuple[str, ...] = ()
    # Review resolutions, keyed by Case.key() (the source hash): "accept:
    # why" admits a case the comparison sent to review; "drop: why" records
    # a flagged case as deliberately not converted.
    reviewed: dict[str, str] = dataclasses.field(default_factory=dict)
    # Per test function, after reading its review items: admit cases whose
    # only disagreement is that the C++ test asserted rejection without a
    # code, or asserted a subset of the codes the compiler reports. Never
    # admits an acceptance/rejection disagreement or a code asserted absent.
    review_policy: dict[str, str] = dataclasses.field(default_factory=dict)
    # Case keys that also carry `//@ format-stable` (a formatter assertion
    # the C++ test made over a source the tool could not take directly).
    format_stable: tuple[str, ...] = ()


FIXTURE_HELPERS = {
    "analyzeTrustedPreludeFixture": Helper("analyze", source_arg=1,
                                           name_arg=0),
    "analyzeTrustedStandalonePreludeFixture": Helper(
        "analyze", source_arg=1, name_arg=0,
        differences=("standalone prelude",)),
    "analyzeEmissionFixture": Helper(
        "accept", dropped=("assertions on the returned MIR/HIR",)),
    # lang::Frontend(...).analyze(name, source, ...): keyed with a dot.
    ".analyze": Helper("analyze", source_arg=1, method=True, name_arg=0),
}

# Per-file helper semantics, written after reading each file's helpers.
SPECS: dict[str, FileSpec] = {
    "protected_member_tests.cpp": FileSpec(
        area="language/protected", topic="Protected members",
        helpers={
            "accept": Helper("accept"),
            "reject": Helper(
                "reject", code_arg=1, default_code="GTI-S2007",
                dropped=(
                    "GTI-S2007 message names protected visibility",
                    "GTI-S2007 primary span is non-empty and in the test file",
                    "GTI-S2007 carries a related declaration span",
                    "rejection happens in semantics (syntaxValid)",
                )),
        },
        format_stable=("dde3e00b6d43086c",),
        reviewed={
            "main:77": "drop: examples/150-protected-events.gti is compiled "
                       "and run by protected_member_runtime",
            "main:199": "drop: the formatted example; protected-section "
                        "formatting is carried by //@ format-stable on "
                        "protected_1, which shares the Base class",
        },
        review_policy={
            "main": "the helper asserts only that its code is present; "
                    "reading Base<bool>'s protected static through another "
                    "instantiation is both GTI-S2039 and GTI-S2007",
        }),
    "shared_ownership_tests.cpp": FileSpec(
        area="stdlib/memory/shared_owner", topic="Shared ownership"),
    "compiler_declarations_tests.cpp": FileSpec(
        area="generics/constraints", topic="Constraints",
        # testInterfaceStaticMethods stays C++: it asserts GTI-S2106 under
        # requireFunctionDefinitions, which check mode does not apply.
        only=("testInferredVoidGenericArguments",
              "testDependentTypeDefaultConstraints",
              "testStructuralClassConstraints",
              "testDependentRequiresArgumentAdmission",
              "testMemberRequirementConcepts",
              "testExactMemberSignatureConcepts",
              "testInterfaceStaticAmbiguity"),
        review_policy={
            name: "the C++ test asserts rejection (or one code of it); the "
                  "compiler's complete code set was read and matches the "
                  "case's stated reason"
            for name in ("testInterfaceStaticAmbiguity",
                         "testMemberRequirementConcepts",
                         "testExactMemberSignatureConcepts",
                         "testStructuralClassConstraints")}),
    "compiler_ownership_tests.cpp": FileSpec(
        area="ownership/stored_references", topic="Ownership",
        # Most named ownership tests assert MIR loan/cleanup structure and
        # wait for MIR snapshots; these three are accept/reject shaped.
        only=("testStoredReferenceLoopInvalidation3",
              "testStoredReferenceLoopInvalidation4",
              "testWritableAndMovementIteratorCapabilities"),
        review_policy={
            name: "the C++ test asserts rejection; the reported code "
                  "(GTI-S2017 invalidated loan, GTI-S2029 unsatisfied "
                  "iterator concept) is the case's stated reason"
            for name in ("testStoredReferenceLoopInvalidation3",
                         "testStoredReferenceLoopInvalidation4",
                         "testWritableAndMovementIteratorCapabilities")}),
    "compiler_constexpr_tests.cpp": FileSpec(
        area="constexpr", topic="Constant evaluation",
        # The heaviest named tests; their fixture mutations, rejection tables
        # and diagnostics snapshots were written by hand from the same file.
        only=("testConstexprLibrary", "testLazyStagingAndCompileTimePanic",
              "testDependentConstexprBranches",
              "testConstexprReadOnlyReferences", "testConstexprResults",
              "testStandardString", "testConstexprAllocation",
              "testConstexprMutation", "testConstexprEnumBuilders",
              "testConstexprOperators", "testConstexprTryable",
              "testConstexprPayloadEnums"),
        areas={"testConstexprLibrary": "constexpr/library",
               "testLazyStagingAndCompileTimePanic": "constexpr/lazy_staging",
               "testDependentConstexprBranches":
                   "constexpr/dependent_branches",
               "testConstexprReadOnlyReferences":
                   "constexpr/readonly_references",
               "testConstexprResults": "constexpr/results",
               "testStandardString": "types/string",
               "testConstexprAllocation": "constexpr/allocation",
               "testConstexprMutation": "constexpr/mutation",
               "testConstexprEnumBuilders": "constexpr/enum_builders",
               "testConstexprOperators": "constexpr/operators"},
        review_policy={
            name: "the C++ test asserts rejection (or one code of it); the "
                  "compiler's complete code set was read and matches the "
                  "case's stated reason"
            for name in ("testConstexprLibrary", "testConstexprResults",
                         "testConstexprEnumBuilders")}),
}


# --------------------------------------------------------------------------
# Case discovery


@dataclasses.dataclass
class Case:
    file: str
    function: str
    line: int
    source: Optional[str]
    expect: str = "unknown"  # accept, reject, unknown
    codes: list[str] = dataclasses.field(default_factory=list)
    absent_codes: list[str] = dataclasses.field(default_factory=list)
    exact_empty: bool = False  # C++ asserted no diagnostics at all
    carried: list[str] = dataclasses.field(default_factory=list)
    dropped: list[str] = dataclasses.field(default_factory=list)
    flags: list[str] = dataclasses.field(default_factory=list)
    message: str = ""
    differences: list[str] = dataclasses.field(default_factory=list)
    # Variables derived from the result: name -> codes it was looked up by.
    aliases: dict[str, list[str]] = dataclasses.field(default_factory=dict)
    # The C++ test formatted this source and asserted the result still
    # analyses (and, usually, that formatting is idempotent).
    format_stable: bool = False
    # Message substrings the C++ test asserted: (code or "", text).
    substrings: list[tuple[str, str]] = dataclasses.field(default_factory=list)
    # Code families asserted with starts_with("GTI-S").
    families: list[str] = dataclasses.field(default_factory=list)

    def key(self) -> str:
        return hashlib.sha256((self.source or "").encode()).hexdigest()[:16]


_ASSERT_CALLS = {"expect", "check", "require", "assert", "CHECK", "EXPECT",
                 "REQUIRE", "expectTrue", "ensure"}
_ACCEPT_FIELDS = ("canGenerateCode", "semanticValid", "hirValid", "mirValid",
                  "valid", "succeeded", "success")


def _calls(tokens: list[Token], helpers: dict[str, Helper]):
    """(index, name, helper, argument token lists) for helper calls."""
    for index, token in enumerate(tokens[:-1]):
        if token.kind != "ident" or tokens[index + 1].text != "(":
            continue
        before = tokens[index - 1].text if index else ""
        method = before in (".", "->")
        helper = helpers.get(("." if method else "") + token.text)
        if not helper:
            continue
        if not helper.method and before == "::" and index >= 2 and \
                tokens[index - 2].text != "std":
            pass
        close = match_close(tokens, index + 1)
        yield index, token.text, helper, split_top(tokens[index + 2:close],
                                                   ","), close


def _looks_like_source(value: str) -> bool:
    return bool(re.search(r"\bmain\s*\(|^\s*(import|class|struct|enum|"
                          r"interface|namespace|union|concept)\s+[\w.]+|"
                          r"\)\s*\{", value, re.M)) and \
        bool(re.search(r"[;{}]", value)) and len(value) > 12


def _calls_any(tokens: list[Token], helpers: dict[str, Helper]) -> bool:
    return any(True for _ in _calls(tokens, helpers))


def _assert_message(tokens: list[Token]) -> str:
    if tokens and tokens[0].text in _ASSERT_CALLS and len(tokens) > 1 and \
            tokens[1].text == "(":
        args = split_top(tokens[2:match_close(tokens, 1)], ",")
        if len(args) >= 2:
            strings = [t.value for t in args[-1] if t.kind == "string"]
            if strings:
                return "".join(strings)
    return ""


def _classify_structural(text: str) -> str:
    lowered = text.lower()
    for needle, label in (
            ("fixes", "fix-its"), ("fix", "fix-its"),
            ("related", "related diagnostics/spans"),
            ("hints", "diagnostic hints"),
            ("primary", "diagnostic spans"), ("span", "diagnostic spans"),
            (".message", "diagnostic message text"),
            ("render", "rendered output"), ("format", "rendered output"),
            ("llvm", "LLVM IR"), (".mir", "MIR structure"),
            ("mir", "MIR structure"), (".hir", "HIR structure"),
            ("instances", "HIR structure"),
            ("semantic", "semantic model structure"),
            ("program", "AST structure"), ("count", "counts"),
            ("size()", "counts")):
        if needle in lowered:
            return label
    return "other assertion"


def interpret(statement_tokens: list[Token], var: str, case: Case) -> bool:
    """Reads one assertion on result `var` into `case`; False if it is not
    an assertion at all (logging)."""
    text = compact(statement_tokens, var)
    # Logging-only statements.
    if re.match(r"^(std::cerr|std::cout|dump\w*\(|print\w*\(|for\(|"
                r"report\w*\()", text):
        return False
    conjuncts: list[str]
    assertion = re.match(r"^(\w+)\((.*)\)$", text)
    if assertion and assertion.group(1) in _ASSERT_CALLS:
        args = split_top(statement_tokens[2:match_close(statement_tokens, 1)],
                         ",")
        condition = args[0] if args else []
        conjuncts = [compact(c, var) for c in split_top(condition, "&&")]
    else:
        conjuncts = [text]
    for conjunct in conjuncts:
        read_conjunct(conjunct, case)
    return True


def read_conjunct(conjunct: str, case: Case) -> None:
    negated = conjunct.startswith("!")
    body = conjunct[1:] if negated else conjunct
    while body.startswith("(") and body.endswith(")"):
        body = body[1:-1]
    if re.fullmatch(r"(lang::)?verifyMirProgram\(\$(\.|->)mir\)\.valid\(\)",
                    body) and not negated:
        case.carried.append("MIR verifies (every check test)")
        return
    if re.match(r"(shippedBackendAccepts|llvmAccepts|llvmEmits|"
                r"gti_test::llvmEmits)\(\$", body):
        case.dropped.append(f"LLVM emission (check proof stops at verified "
                            f"MIR and the LoweredProgram): {conjunct[:80]}")
        return
    text = re.fullmatch(r'hasDiagnostic\(\$(\.|->)diagnostics,("(?:[^"\\]|\\.)*")\)',
                        body)
    if text and not negated:
        case.substrings.append(("", json.loads(text.group(2))))
        case.carried.append(f"message contains {text.group(2)}")
        set_expect(case, "reject", "diagnostic text asserted")
        return
    aliased = re.fullmatch(r'(\w+)->message\.find\(("(?:[^"\\]|\\.)*")\)'
                           r'!=std::string::npos', body)
    if aliased and not negated and case.aliases.get(aliased.group(1)):
        case.substrings.append((case.aliases[aliased.group(1)][0],
                                json.loads(aliased.group(2))))
        case.carried.append(f"{case.aliases[aliased.group(1)][0]} message "
                            f"contains {aliased.group(2)}")
        return
    alias = re.fullmatch(r"(\w+)(!=nullptr|==nullptr)?", body)
    if alias and case.aliases.get(alias.group(1)):
        absent = negated or alias.group(2) == "==nullptr"
        for code in case.aliases[alias.group(1)]:
            target = case.absent_codes if absent else case.codes
            if code not in target:
                target.append(code)
        case.carried.append(("absence of " if absent else "") +
                            "/".join(case.aliases[alias.group(1)]))
        if not absent:
            set_expect(case, "reject", "code asserted")
        return
    accept = re.fullmatch(
        r"\$(\.|->)(" + "|".join(_ACCEPT_FIELDS) + r")(\(\))?", body)
    if accept:
        if negated:
            set_expect(case, "reject", f"!{accept.group(2)}")
        else:
            set_expect(case, "accept", accept.group(2))
        return
    if re.fullmatch(r"\$(\.|->)syntaxValid", body):
        case.carried.append("parses" if not negated else "does not parse")
        if negated:
            set_expect(case, "reject", "!syntaxValid")
        return
    family = re.search(r'starts_with\("(GTI-[A-Z]*)"\)', body)
    if family and "$" in body and not re.search(r'"GTI-[A-Z]+[0-9]+"', body):
        if negated or "none_of" in body:
            case.dropped.append(f"absence of any {family.group(1)} code: "
                                f"{conjunct[:100]}")
        else:
            case.families.append(family.group(1))
            case.carried.append(f"some {family.group(1)}* error")
            set_expect(case, "reject", "code family asserted")
        return
    if re.fullmatch(r"\$(\.|->)diagnostics\.empty\(\)", body):
        if negated:
            set_expect(case, "reject", "diagnostics non-empty")
        else:
            set_expect(case, "accept", "diagnostics.empty()")
            case.exact_empty = True
        return
    codes = re.findall(r'"(GTI-[A-Z]+[0-9]+)"', body)
    if codes and "$" in body:
        absent = negated or "none_of" in body or re.search(
            r"(==0|==0u|\.end\(\)$|==nullptr)$", body) is not None
        for code in codes:
            target = case.absent_codes if absent else case.codes
            if code not in target:
                target.append(code)
        count = re.search(r"countDiagnosticCode\(.*\)==(\d+)", body)
        case.carried.append(("absence of " if absent else "") +
                            "/".join(codes) + (f" x{count.group(1)}"
                                               if count else ""))
        if not absent:
            set_expect(case, "reject", "code asserted")
        return
    if "$" in body or not case.function or any(
            re.search(rf"(?<!\w){re.escape(name)}(?!\w)", body)
            for name in case.aliases):
        case.dropped.append(f"{_classify_structural(body)}: {conjunct}")


def set_expect(case: Case, kind: str, why: str) -> None:
    if case.expect in ("unknown", kind):
        case.expect = kind
        case.carried.append(why)
    else:
        case.flags.append(f"contradictory expectation ({case.expect} vs "
                          f"{kind}: {why})")


def declared_name(tokens: list[Token]) -> Optional[str]:
    """The variable a declaration statement introduces, if it is one."""
    for index, token in enumerate(tokens):
        if token.kind == "punct" and token.text in _OPEN:
            if token.text == "{" and index >= 2 and \
                    tokens[index - 1].kind == "ident" and \
                    tokens[index - 2].kind in ("ident", "punct") and \
                    tokens[index - 2].text not in ("=", "(", ",", "return"):
                return tokens[index - 1].text
            return None
        if token.kind == "punct" and token.text == "=":
            if index >= 2 and tokens[index - 1].kind == "ident" and (
                    tokens[index - 2].kind == "ident" or
                    tokens[index - 2].text in (">", "*", "&", "&&")):
                return tokens[index - 1].text
            return None
    return None


def assigned_name(tokens: list[Token]) -> Optional[str]:
    if len(tokens) >= 2 and tokens[0].kind == "ident" and \
            tokens[1].text == "=":
        return tokens[0].text
    return None


def _references(tokens: list[Token], name: str) -> bool:
    return any(t.kind == "ident" and t.text == name for t in tokens)


def _walk(statements: list[Stmt]):
    for statement in statements:
        yield statement
        yield from _walk(statement.children)


def _all_tokens(statement: Stmt) -> list[Token]:
    tokens = list(statement.tokens)
    for child in statement.children:
        tokens.extend(_all_tokens(child))
    return tokens


class Extractor:
    def __init__(self, unit: Unit, spec: FileSpec, extra: list[Unit]):
        self.unit = unit
        self.spec = spec
        self.helpers = dict(FIXTURE_HELPERS)
        self.helpers.update(spec.helpers)
        self.evaluator = Evaluator(unit, extra)
        self._derive_file_helpers()
        self.cases: list[Case] = []
        self.unpaired: list[str] = []
        self.consumed: set[int] = set()

    def _derive_file_helpers(self) -> None:
        changed = True
        while changed:
            changed = False
            for name, function in self.unit.functions.items():
                if name in self.helpers or name == "main":
                    continue
                helper = self.derive_helper(function.params, function.body,
                                            self.evaluator.global_env)
                if helper:
                    self.helpers[name] = helper
                    changed = True

    def derive_helper(self, params: list[Token], body: list[Token],
                      env: Env) -> Optional[Helper]:
        """An `analyze` helper: a callable returning the analysis of one of
        its string parameters, possibly with fixed text around it."""
        if not any(t.text == "return" for t in body):
            return None
        if any(t.text in _ASSERT_CALLS or t.text == "failures" for t in body):
            return None  # it asserts; its meaning needs a FileSpec entry
        inner = Env(env)
        names = []
        for index, part in enumerate(split_top(params, ",")):
            words = [t.text for t in part]
            if "=" in words:
                part = part[:words.index("=")]
            idents = [t for t in part if t.kind == "ident"]
            if not idents:
                return None
            names.append(idents[-1].text)
            if any(w in ("string", "string_view") for w in words) or \
                    ("char" in words and "*" in words):
                inner.strings[idents[-1].text] = f"\0P{index}\0"
        for statement in _walk(parse_block(body)):
            if statement.kind == "expr":
                self.evaluator.declare(statement.tokens, inner, set())
        for _, callee, helper, args, _close in _calls(body, self.helpers):
            if helper.kind != "analyze" or helper.source_arg >= len(args):
                continue
            value = self.evaluator.evaluate(args[helper.source_arg], inner)
            if value is None:
                continue
            markers = re.findall(r"\0P(\d+)\0", value)
            if len(markers) != 1:
                continue
            prefix, suffix = re.split(r"\0P\d+\0", value)
            differences = list(helper.differences)
            words = {t.text for t in body}
            if helper.method and len(args) <= 2:
                differences.append("analysed without the standard library "
                                   "prelude")
            for option in ("stopAfter", "target", "runtimeCapabilities",
                           "FrontendOptions", "rawPointerAudit", "options"):
                if option in words:
                    differences.append(f"frontend option `{option}`")
            return Helper("analyze", source_arg=int(markers[0]),
                          prefix=helper.prefix + prefix,
                          suffix=suffix + helper.suffix,
                          differences=tuple(differences))
        return None

    def _local_helper(self, name: str, tokens: list[Token], env: Env) -> bool:
        texts = [t.text for t in tokens]
        if "[" not in texts or "=" not in texts:
            return False
        start = texts.index("[")
        if start != texts.index("=") + 1:
            return False
        close = match_close(tokens, start)
        if close + 1 >= len(tokens) or tokens[close + 1].text != "(":
            return False
        params_close = match_close(tokens, close + 1)
        brace = params_close + 1
        while brace < len(tokens) and tokens[brace].text != "{":
            brace += 1
        if brace >= len(tokens):
            return False
        body = tokens[brace + 1:match_close(tokens, brace)]
        if name in self.spec.helpers:
            self.helpers[name] = self.spec.helpers[name]
            return True
        helper = self.derive_helper(tokens[close + 2:params_close], body, env)
        if helper:
            self.helpers[name] = helper
            return True
        if _calls_any(body, self.helpers):
            self.unpaired.append(f"line {tokens[0].line}: local helper "
                                 f"`{name}` analyses source in a way the "
                                 "tool does not model; its calls are not "
                                 "extracted")
            return True
        return False

    def run(self) -> list[Case]:
        for function in self.unit.functions.values():
            if function.name in self.helpers or function.name == "main" and \
                    self.spec.only:
                continue
            if self.spec.only and function.name not in self.spec.only:
                continue
            if function.name in self.spec.skip:
                continue
            body = parse_block(function.body)
            env = Env(self.evaluator.global_env)
            file_helpers = dict(self.helpers)
            self._block(function, body, env, loops=[])
            self.helpers = file_helpers  # local lambdas end with the function
        self._uncovered_literals()
        return self.cases

    def _uncovered_literals(self) -> None:
        """Source-like literals no case used: the safety net for idioms the
        tool does not model."""
        sources = [case.source for case in self.cases if case.source]
        selected = [f for f in self.unit.functions.values()
                    if (not self.spec.only or f.name in self.spec.only) and
                    f.name not in self.spec.skip and f.name not in self.helpers]
        for function in selected:
            for token in function.body:
                if token.kind != "string" or not _looks_like_source(
                        token.value):
                    continue
                piece = token.value.strip()
                if not any(piece in source for source in sources):
                    self.unpaired.append(
                        f"{function.name}:{token.line}: source literal used "
                        f"by no extracted case: {piece[:60]!r}")

    def _block(self, function: Function, statements: list[Stmt], env: Env,
               loops: list) -> None:
        for position, statement in enumerate(statements):
            if statement.kind in ("expr", "return"):
                self._expr(function, statement, statements[position + 1:],
                           env, loops)
            elif statement.kind == "for":
                self._for(function, statement, env, loops)
            elif statement.kind in ("while", "do"):
                self._block(function, statement.children, Env(env),
                            loops + [None])
            elif statement.kind == "if":
                self._scan_calls(function, statement, statement.tokens, [],
                                 env, loops)
                self._block(function, statement.children, Env(env), loops)
            else:
                self._block(function, statement.children, Env(env), loops)

    def _for(self, function, statement, env, loops):
        header = statement.tokens
        parts = split_top(header, ":")
        if len(parts) == 2 and ";" not in [t.text for t in header]:
            variable = [t for t in parts[0] if t.kind == "ident"][-1].text
            values: Optional[list[str]] = None
            domain = parts[1]
            if domain and domain[0].text == "{":
                values = []
                for part in split_list(domain[1:match_close(domain, 0)]):
                    value = self.evaluator.evaluate(part, env)
                    if value is None:
                        values = None
                        break
                    values.append(value)
            elif len(domain) == 1 and domain[0].kind == "ident":
                found = env.lookup(domain[0].text)
                if found and found[0] == "list":
                    values = found[1]
            if values is not None:
                for value in values:
                    inner = Env(env)
                    inner.strings[variable] = value
                    self._block(function, statement.children, inner,
                                loops + [("range", variable)])
                return
        self._block(function, statement.children, Env(env), loops + [None])

    def _expr(self, function, statement, rest, env, loops):
        tokens = statement.tokens
        self.evaluator.declare(tokens, env, set())
        declared = declared_name(tokens)
        if declared and self._local_helper(declared, tokens, env):
            return
        before = len(self.cases)
        self._scan_calls(function, statement, tokens, rest, env, loops,
                         declared)
        if len(self.cases) == before and id(statement) not in self.consumed \
                and tokens and tokens[0].text in _ASSERT_CALLS:
            self.unpaired.append(f"{function.name}:{statement.line}: "
                                 f"{_classify_structural(compact(tokens))}: "
                                 f"{compact(tokens)[:140]}")

    def _scan_calls(self, function, statement, tokens, rest, env, loops,
                    declared=None):
        calls = list(_calls(tokens, self.helpers))
        for index, name, helper, args, close in calls:
            case = Case(self.unit.path.name, function.name, tokens[index].line,
                        None)
            case.differences.extend(helper.differences)
            case.dropped.extend(f"helper `{name}`: {item}"
                                for item in helper.dropped)
            if helper.source_arg >= len(args):
                case.flags.append(f"`{name}` call without a source argument")
                self.cases.append(case)
                continue
            source = self.evaluator.evaluate(args[helper.source_arg], env)
            if any(loop is None for loop in loops):
                case.flags.append("inside a loop over computed values")
            if source is None:
                case.flags.append("source is not a compile-time string: " +
                                  compact(args[helper.source_arg])[:120])
            else:
                if source.startswith(FORMATTED):
                    source = source[len(FORMATTED):]
                    case.format_stable = True
                    case.carried.append("formatted copy analyses "
                                        "(//@ format-stable)")
                if FORMATTED in source:
                    case.flags.append("formatter output inside a larger "
                                      "source")
                case.source = helper.prefix + source + helper.suffix
            if len(args) > helper.source_arg + 1 and helper.kind == "analyze":
                extra = args[helper.source_arg + 1:]
                if any(t.text == "{" for part in extra for t in part[:1]) and \
                        any(t.kind == "string" for part in extra for t in part):
                    case.flags.append("analysis has extra source files or "
                                      "options: " + compact(
                                          [t for p in extra for t in p])[:120])
            if helper.kind == "accept":
                set_expect(case, "accept", f"helper `{name}`")
            elif helper.kind == "reject":
                code = None
                if helper.code_arg is not None and helper.code_arg < len(args):
                    code = self.evaluator.evaluate(args[helper.code_arg], env)
                    if code is None:
                        case.flags.append("reject code is not a constant")
                code = code or helper.default_code
                if code:
                    case.codes.append(code)
                set_expect(case, "reject", f"helper `{name}`")
            else:
                # The analysis result: the declared variable, or the
                # enclosing assertion when the call is nested in one.
                whole_init = declared and close == len(tokens) - 1
                if whole_init:
                    self._follow(case, declared, rest)
                else:
                    replaced = tokens[:index] + [Token("ident", "__case", 0)] \
                        + tokens[close + 1:]
                    message = _assert_message(tokens)
                    case.message = case.message or message
                    if declared and not whole_init:
                        case.flags.append("result used inside a larger "
                                          "declaration: " +
                                          compact(tokens)[:120])
                    interpret(replaced, "__case", case)
            if not case.message:
                case.message = _assert_message(tokens)
            self.cases.append(case)

    def _follow(self, case: Case, var: str, rest: list[Stmt]) -> None:
        for statement in rest:
            tokens = statement.tokens
            if statement.kind in ("expr", "return") and tokens and \
                    assigned_name(tokens) == var:
                return  # reassigned
            if statement.kind in ("expr", "return") and \
                    _references(tokens, var) and declared_name(tokens) and \
                    not _calls_any(tokens, self.helpers):
                # A value derived from the result: a diagnostic looked up by
                # code is an assertion when tested; anything else is
                # structure.
                alias = declared_name(tokens)
                codes = re.findall(r'"(GTI-[A-Z]+[0-9]+)"', compact(tokens))
                case.aliases[alias] = codes
                self.consumed.add(id(statement))
                if not codes:
                    case.dropped.append(
                        f"{_classify_structural(compact(tokens))}: derived "
                        f"value {compact(tokens)[:100]}")
                continue
            if statement.kind in ("expr", "return"):
                if _references(tokens, var) or any(
                        _references(tokens, a) for a in case.aliases):
                    self.consumed.add(id(statement))
                    case.message = case.message or _assert_message(tokens)
                    interpret(tokens, var, case)
                continue
            if statement.kind == "if" and _references(statement.tokens, var):
                self._follow_if(case, var, statement)
                continue
            if _references(_all_tokens(statement), var):
                for inner in _walk(statement.children):
                    if inner.kind in ("expr", "return") and \
                            _references(inner.tokens, var):
                        interpret(inner.tokens, var, case)
                    elif inner.kind not in ("expr", "return", "block") and \
                            _references(inner.tokens, var):
                        case.dropped.append(
                            f"{_classify_structural(compact(inner.tokens))}: "
                            f"{inner.kind} ({compact(inner.tokens)[:100]})")

    def _follow_if(self, case: Case, var: str, statement: Stmt) -> None:
        # `if (!r.canGenerateCode()) { dump(r); }` is logging; any branch
        # that fails the test turns the condition into an assertion.
        body = [_all_tokens(child) for child in statement.children]
        fails = any(any(t.text in ("failures", "throw", "expect", "check",
                                   "require", "fail") for t in tokens)
                    for tokens in body)
        condition = compact(statement.tokens)
        if not fails:
            return
        for inner in _walk(statement.children):
            if inner.kind in ("expr", "return") and _references(inner.tokens,
                                                                var):
                interpret(inner.tokens, var, case)
        if not any(_references(t, var) for t in body):
            case.dropped.append(
                f"{_classify_structural(condition)}: guarded by "
                f"if ({condition[:100]})")


# --------------------------------------------------------------------------
# Emission


def snake(name: str) -> str:
    name = re.sub(r"^(test|check)", "", name)
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower().strip("_")


def header_line(case: Case, spec: FileSpec) -> str:
    message = case.message.strip().rstrip(".:").rstrip()
    if not message:
        message = "the program is accepted" if case.expect == "accept" else \
            "rejected with " + ", ".join(case.codes) if case.codes else \
            "analysis result"
    words = snake(case.function).replace("_", " ")
    if case.function == "main" or not words:
        words = spec.topic or spec.area
    text = f"{words}: {message}" if message else words
    text = text[0].upper() + text[1:]
    return "// " + text.replace("\n", " ") + "."


def render(case: Case, header: str) -> str:
    source = case.source or ""
    if source.startswith("\n"):
        source = source[1:]
    source = source.rstrip() + "\n"
    lines = [header]
    if case.expect == "accept":
        lines.append("//@ check-pass")
    if case.format_stable:
        lines.append("//@ format-stable")
    return "\n".join(lines) + "\n" + source


@dataclasses.dataclass
class Planned:
    case: Case
    relative: str  # under tests/ui (or tests/slow)
    duplicates: list[Case] = dataclasses.field(default_factory=list)


def plan(cases: list[Case], spec: FileSpec, existing: dict[str, str]):
    planned: list[Planned] = []
    by_source: dict[str, Planned] = {}
    flagged: list[Case] = []
    counters: dict[str, int] = {}
    totals: dict[str, int] = {}
    for case in cases:
        totals[case.function] = totals.get(case.function, 0) + 1
    for case in cases:
        if case.source is None or case.flags:
            flagged.append(case)
            continue
        # Compared without surrounding blank space: a rendered file always
        # ends in one newline whatever the literal did.
        body = case.source.strip()
        if body in existing:
            case.flags.append(f"duplicate of existing {existing[body]}")
            flagged.append(case)
            continue
        if body in by_source:
            kept = by_source[body]
            kept.duplicates.append(case)
            if case.format_stable:
                kept.case.format_stable = True
                kept.case.carried.append("formatted copy analyses "
                                         "(//@ format-stable)")
            if kept.case.expect == "unknown":
                kept.case.expect = case.expect
            continue
        if case.key() in spec.format_stable:
            case.format_stable = True
            case.carried.append("formatter fixed point (//@ format-stable)")
        stem = snake(case.function)
        if case.function == "main" or not stem:
            stem = spec.area.rsplit("/", 1)[-1]
        counters[stem] = counters.get(stem, 0) + 1
        name = stem if totals[case.function] == 1 else \
            f"{stem}_{counters[stem]}"
        area = spec.areas.get(case.function, spec.area)
        item = Planned(case, f"{area}/{name}.gti")
        by_source[body] = item
        planned.append(item)
    return planned, flagged


def existing_bodies(roots: list[Path]) -> dict[str, str]:
    bodies = {}
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*.gti"):
            text = path.read_text()
            lines = [line for line in text.splitlines(keepends=True)]
            while lines and lines[0].startswith("//"):
                lines.pop(0)
            body = re.sub(r"\s*//~.*", "", "".join(lines))
            bodies[body.strip()] = str(path.relative_to(ROOT))
    return bodies


_ANNOTATION = re.compile(r"//~(\^*|v|\||\?)?\s*(ERROR|WARN)\s+(GTI-[A-Z]+\d+)")


def annotations(text: str) -> list[tuple[str, str]]:
    return [(m.group(2), m.group(3)) for m in _ANNOTATION.finditer(text)]


def run_runner(runner: Path, root: Path, *args: str) -> tuple[int, str, list]:
    environment = dict(os.environ)
    environment.pop("CI", None)
    json_path = root.parent / (root.name + ".json")
    result = subprocess.run(
        [str(runner), "--root", str(root), "--json", str(json_path), *args],
        capture_output=True, text=True, env=environment, timeout=3600)
    records = []
    if json_path.exists():
        for line in json_path.read_text().splitlines():
            if line.strip():
                records.append(json.loads(line))
    return result.returncode, result.stdout + result.stderr, records


def _asserted_subset(item: Planned, blessed_text: str) -> bool:
    errors = {code for severity, code in annotations(blessed_text)
              if severity == "ERROR"}
    return bool(item.case.codes) and set(item.case.codes) <= errors


def decide(item: Planned, blessed_text: str) -> Optional[str]:
    """None when the compiler agrees with the C++ expectation; else why not."""
    case = item.case
    found = annotations(blessed_text)
    errors = sorted({code for severity, code in found if severity == "ERROR"})
    warnings = sorted({code for severity, code in found if severity == "WARN"})
    if case.expect == "accept":
        if errors:
            return f"C++ test accepts; compiler reports {errors}"
        if warnings and case.exact_empty:
            return f"C++ test asserts no diagnostics; compiler warns {warnings}"
        return None
    if case.expect == "reject":
        asserted = sorted(set(case.codes))
        if not errors:
            return f"C++ test rejects with {asserted}; compiler accepts"
        if set(case.absent_codes) & set(errors):
            return f"compiler reports codes the C++ test asserts absent: " \
                   f"{sorted(set(case.absent_codes) & set(errors))}"
        if not asserted and case.families:
            if all(any(code.startswith(f) for code in errors)
                   for f in case.families) and all(
                       any(code.startswith(f) for f in case.families)
                       for code in errors):
                return None
            return f"C++ test asserts {case.families} codes; compiler " \
                   f"reports {errors}"
        if not asserted:
            return f"C++ test asserts rejection without a code; compiler " \
                   f"reports {errors}"
        if set(case.absent_codes) & set(errors):
            return f"compiler reports codes the C++ test asserts absent: " \
                   f"{sorted(set(case.absent_codes) & set(errors))}"
        if errors != asserted:
            return f"code set differs: C++ asserts {asserted}, compiler " \
                   f"reports {errors}"
        return None
    if errors:
        return f"C++ test asserts no acceptance or code; compiler reports " \
               f"{errors}"
    return "C++ test asserts only structure; compiler accepts (review)"


def attach_substrings(runner: Path, path: Path, case: Case) -> None:
    """Carries asserted message text as `//~ ERROR CODE text`, on the first
    annotation of the code for which the test still passes."""
    for code, text in case.substrings:
        if "\n" in text or "//~" in text or not text.strip():
            case.dropped.append(f"diagnostic message text {text!r}")
            continue
        pattern = re.compile(r"(//~\S*\s*ERROR\s+" +
                             (re.escape(code) if code else
                              r"GTI-[A-Z]+[0-9]+") + r")(?=\s*(//~|$))")
        lines = path.read_text().splitlines(keepends=True)
        original = "".join(lines)
        placed = False
        for index, line in enumerate(lines):
            new = pattern.sub(lambda m: m.group(1) + " " + text.strip(),
                              line.rstrip("\n"), count=1)
            if new == line.rstrip("\n"):
                continue
            path.write_text("".join(lines[:index] + [new + "\n"] +
                                    lines[index + 1:]))
            _, _, records = run_runner(runner, path.parent, path.name)
            if records and all(r["status"] == "PASS" for r in records):
                placed = True
                case.carried.append(f"message text {text!r} on its "
                                    "annotation")
                break
            path.write_text(original)
        if not placed:
            case.dropped.append(f"diagnostic message text {text!r} (no "
                                "annotation's diagnostic carries it)")


def extract(args) -> int:
    path = Path(args.file)
    spec = SPECS.get(path.name)
    if not spec:
        sys.exit(f"no helper specification for {path.name}; read its helpers "
                 "and add one to SPECS")
    extra = [parse_unit(ROOT / "tests/compiler_test_fixture.h")] \
        if "compiler_test_fixture.h" in path.read_text() else []
    unit = parse_unit(path)
    extractor = Extractor(unit, spec, extra)
    cases = extractor.run()
    existing = existing_bodies([ROOT / "tests/ui", ROOT / "tests/slow"])
    planned, flagged = plan(cases, spec, existing)
    staging = Path(args.staging or ROOT / "build-extract" / path.stem)
    if staging.exists():
        shutil.rmtree(staging)
    stage_root = staging / "ui"
    for item in planned:
        target = stage_root / item.relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(render(item.case, header_line(item.case, spec)))
    runner = Path(args.gti_test)
    run_runner(runner, stage_root, "--bless", "--jobs", str(args.jobs))
    report = {"file": path.name, "cases": len(cases),
              "unpaired": extractor.unpaired, "accepted": [],
              "review": [], "flagged": [], "deduplicated": []}
    for item in planned:
        target = stage_root / item.relative
        reason = decide(item, target.read_text())
        resolution = spec.reviewed.get(item.case.key(), "")
        policy = spec.review_policy.get(item.case.function)
        if reason and policy and not resolution and (
                reason.startswith("C++ test asserts rejection without a code")
                or (reason.startswith("code set differs") and
                    _asserted_subset(item, target.read_text()))):
            resolution = "accept: " + policy
        if reason and resolution.startswith("accept:"):
            item.case.carried.append(f"review: {resolution[7:].strip()} "
                                     f"({reason})")
            reason = None
        entry = {"path": item.relative, "key": item.case.key(),
                 "function": item.case.function,
                 "line": item.case.line, "expect": item.case.expect,
                 "codes": item.case.codes, "carried": item.case.carried,
                 "dropped": item.case.dropped,
                 "differences": item.case.differences,
                 "duplicates": [f"{d.function}:{d.line}"
                                for d in item.duplicates]}
        for duplicate in item.duplicates:
            report["deduplicated"].append(
                {"kept": item.relative,
                 "case": f"{duplicate.function}:{duplicate.line}",
                 "expect": duplicate.expect, "codes": duplicate.codes,
                 "dropped": duplicate.dropped})
        if reason:
            entry["reason"] = reason
            report["review"].append(entry)
        else:
            if item.case.substrings:
                attach_substrings(runner, target, item.case)
                entry["carried"] = item.case.carried
                entry["dropped"] = item.case.dropped
            report["accepted"].append(entry)
    for case in flagged:
        report["flagged"].append({"key": case.key() if case.source else None,
                                  "resolution": spec.reviewed.get(
                                      case.key() if case.source else
                                      f"{case.function}:{case.line}", ""),
                                  "function": case.function,
                                  "line": case.line, "flags": case.flags,
                                  "expect": case.expect, "codes": case.codes,
                                  "dropped": case.dropped})
    # Timing and agreement, without bless, over the accepted cases only.
    review_paths = {entry["path"] for entry in report["review"]}
    for relative in review_paths:
        (stage_root / relative).rename(
            (stage_root / relative).with_suffix(".review"))
    code, output, records = run_runner(runner, stage_root, "--jobs",
                                       str(args.jobs))
    report["timing"] = {r["path"]: r["cpu"] for r in records}
    report["runner_status"] = {r["path"]: r["status"] for r in records
                               if r["status"] != "PASS"}
    (staging / "report.json").write_text(json.dumps(report, indent=1) + "\n")
    print(f"{path.name}: {len(cases)} cases, {len(report['accepted'])} "
          f"accepted, {len(report['review'])} for review, "
          f"{len(report['flagged'])} flagged, "
          f"{len(report['deduplicated'])} deduplicated; report "
          f"{staging / 'report.json'}")
    if args.apply:
        for entry in report["accepted"]:
            if entry["path"] in report["runner_status"]:
                continue
            source = stage_root / entry["path"]
            target = ROOT / "tests/ui" / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    return 0


def print_cases(args) -> int:
    path = Path(args.file)
    spec = SPECS.get(path.name) or FileSpec("unknown", {})
    extra = [parse_unit(ROOT / "tests/compiler_test_fixture.h")] \
        if "compiler_test_fixture.h" in path.read_text() else []
    for case in Extractor(parse_unit(path), spec, extra).run():
        state = "FLAG " + "; ".join(case.flags) if case.flags else case.expect
        print(f"{case.function}:{case.line} [{state}] codes={case.codes} "
              f"key={case.key() if case.source else '-'}")
        for item in case.carried:
            print(f"    carried: {item}")
        for item in case.dropped:
            print(f"    dropped: {item}")
    return 0


# --------------------------------------------------------------------------
# Survey


def survey(args) -> int:
    files = [Path(f) for f in args.files] or sorted(
        (ROOT / "tests").glob("*_tests.cpp"))
    cmake = (ROOT / "tests/CMakeLists.txt").read_text()
    fixture_frontend: set[str] = set()
    for header in ("compiler_test_fixture.h", "mir_test_fixture.h",
                   "private_stdlib_fixture.h", "lowered_program_test_support.h",
                   "llvm_backend_test_support.h", "driver_test_support.h"):
        if (ROOT / "tests" / header).exists():
            for name, f in parse_unit(ROOT / "tests" / header).functions.items():
                if _calls_frontend(f, fixture_frontend):
                    fixture_frontend.add(name)
    for path in files:
        text = path.read_text(errors="replace")
        try:
            unit = parse_unit(path, text)
        except LexError as error:
            print(f"{path.name}\terror\t{error}")
            continue
        frontend = set(fixture_frontend)
        changed = True
        while changed:
            changed = False
            for name, function in unit.functions.items():
                if name not in frontend and _calls_frontend(function, frontend):
                    frontend.add(name)
                    changed = True
        literals = sum(1 for t in unit.tokens if t.kind == "string" and
                       re.search(r"\bmain\s*\(|^\s*(import|class|struct|int|"
                                 r"void|enum|namespace)\b", t.value, re.M))
        target = re.search(rf"add_executable\(\s*\"?(\S+?)\"?\s+[^)]*"
                           rf"{re.escape(path.name)}", cmake)
        links = ""
        if target:
            link = re.search(rf"target_link_libraries\(\s*\"?{re.escape(target.group(1))}\"?\s+PRIVATE\s+([^)]*)\)", cmake)
            links = " ".join(link.group(1).split()) if link else ""
        reads_files = bool(re.search(r"ifstream|readFile|read_text|"
                                     r"examples/|tests/fixtures", text))
        mutates = bool(re.search(r"MirEditor|mir_editor|\bmutat(e|ion)", text))
        syntax_only = not frontend & _called(unit) and bool(
            re.search(r"\b(Lexer|Parser|formatSource|Formatter)\b", text))
        if "cpp_backend" in links or "gti_cpp" in links:
            kind = "cpp-backend"
        elif frontend & _called(unit) and literals:
            kind = "convert"
        elif frontend & _called(unit) and (reads_files or mutates):
            kind = "mutation-or-census"
        elif syntax_only and literals:
            kind = "convert-syntax"
        elif frontend & _called(unit):
            kind = "convert-review"
        else:
            kind = "keep-as-unit"
        helpers = sorted(name for name in frontend & _called(unit)
                         if name not in ("analyze",) or True)
        print(f"{path.name}\t{kind}\tliterals={literals}\t"
              f"helpers={','.join(helpers)}\treads_files={int(reads_files)}"
              f"\tmutates={int(mutates)}\tlinks={links}")
    return 0


def _called(unit: Unit) -> set[str]:
    return {t.text for i, t in enumerate(unit.tokens[:-1])
            if t.kind == "ident" and unit.tokens[i + 1].text == "("}


def _calls_frontend(function: Function, known: set[str]) -> bool:
    texts = [t.text for t in function.body]
    if "Frontend" in texts and "analyze" in texts:
        return True
    if "compileWithBackend" in texts or "analyzeProgram" in texts:
        return True
    return any(t.kind == "ident" and t.text in known and i + 1 < len(
        function.body) and function.body[i + 1].text == "("
        for i, t in enumerate(function.body))


def remove(args) -> int:
    """Deletes converted named tests: their functions, their kTests entries
    and their compiler_test_manifest.json entries."""
    path = Path(args.file)
    text = path.read_text()
    unit = parse_unit(path, text)
    lines = text.splitlines(keepends=True)
    doomed: set[int] = set()
    for name in args.names:
        function = unit.functions.get(name)
        if not function:
            sys.exit(f"{path.name}: no function {name}")
        start = function.start_line - 1
        while start > 0 and lines[start - 1].lstrip().startswith("//"):
            start -= 1
        doomed.update(range(start, function.end_line))
        if function.end_line < len(lines) and not lines[function.end_line].strip():
            doomed.add(function.end_line)
    kept = [line for number, line in enumerate(lines) if number not in doomed]
    result = "".join(kept)
    for name in args.names:
        entry = re.compile(r'\n\s*\{"' + re.escape(name) + r'",\s*' +
                           re.escape(name) + r'\},')
        result, count = entry.subn("", result)
        if count != 1:
            sys.exit(f"{path.name}: kTests entry for {name} not found")
    path.write_text(result)
    manifest = ROOT / "tests/compiler_test_manifest.json"
    data = json.loads(manifest.read_text())
    before = len(data["tests"])
    data["tests"] = [test for test in data["tests"]
                     if test["name"] not in set(args.names)]
    if before - len(data["tests"]) != len(args.names):
        sys.exit("manifest entries missing for some names")
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    s = commands.add_parser("survey")
    s.add_argument("files", nargs="*")
    c = commands.add_parser("cases")
    c.add_argument("file")
    e = commands.add_parser("extract")
    e.add_argument("file")
    e.add_argument("--gti-test", required=True)
    e.add_argument("--staging")
    e.add_argument("--jobs", type=int, default=4)
    e.add_argument("--apply", action="store_true")
    r = commands.add_parser("remove")
    r.add_argument("file")
    r.add_argument("names", nargs="+")
    args = parser.parse_args()
    return {"survey": survey, "cases": print_cases, "extract": extract,
            "remove": remove}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
