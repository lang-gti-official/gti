#!/usr/bin/env python3

"""Correctness probes for `gti_lsp`: position encoding and edit staleness.

Two properties, both exact and falsifiable, both invisible to an ASCII-only
single-shot test.

**positions** — the server advertises `positionEncoding: utf-16`, so every
range it returns is measured in UTF-16 code units, not bytes and not
codepoints. Those three agree for ASCII and disagree everywhere else. Each
case puts a different width of character *before* a declaration on the same
line, because only a same-line prefix shifts a column, then checks that
slicing the line by the returned range reproduces the declaration exactly:

    ascii   1 byte,  1 UTF-16 unit   — the control; a failure here is unrelated
    latin1  2 bytes, 1 UTF-16 unit   — bytes disagree with UTF-16
    cjk     3 bytes, 1 UTF-16 unit   — bytes disagree further
    emoji   4 bytes, 2 UTF-16 units  — a surrogate pair; codepoints disagree too

A byte-counting server fails `latin1` onward; a codepoint-counting server
passes those and fails only `emoji`, which is why the emoji case matters most.

**staleness** — the server reuses frontend source units across edits, so the
hazard is answering from a snapshot the user has already edited away. This
opens a document, renames its only declaration through `didChange`, and asks
again: the old name must be gone and the new one present.

    python3 scripts/debug/lsp_probe.py                  # every check
    python3 scripts/debug/lsp_probe.py --check staleness
    python3 scripts/debug/lsp_probe.py --server build/gti_lsp

Requests are sequenced rather than piped in one burst. That is not a detail:
`didChange` cancels any in-flight request with `-32801 ContentModified`, which
is correct server behaviour, so a burst-written probe races its own edit and
reads an error where it expected a result.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent

DECLARATION = "int32_t alpha() { return 1; }"

# (name, prefix placed in a block comment before the declaration)
CASES = [
    ("ascii", "xx"),
    ("latin1", "café"),
    ("cjk", "日本語"),
    ("emoji", "🎺🎻"),
]


def document(prefix: str) -> str:
    """One declaration preceded on the same line by `prefix` inside a comment."""
    return f"/* {prefix} */ {DECLARATION}\n"


def frame(message: dict) -> bytes:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload


def slice_utf16(line: str, start: int, end: int) -> str:
    """The text between two UTF-16 code-unit offsets on one line."""
    encoded = line.encode("utf-16-le")
    return encoded[start * 2:end * 2].decode("utf-16-le", errors="replace")


def symbol_entries(result) -> list[dict]:
    """Flatten a documentSymbol reply.

    The server answers with flat `SymbolInformation` (a `location.range`),
    but the protocol also permits hierarchical `DocumentSymbol` (a `range`
    plus `selectionRange` and `children`). Accept either, and normalise to
    `(name, range)` so a future switch between the two does not silently turn
    this probe into a no-op — which is exactly what it did when it looked for
    `selectionRange` alone and skipped every symbol it was given.
    """
    entries: list[dict] = []

    def walk(items) -> None:
        for item in items or []:
            span = (item.get("selectionRange") or item.get("range")
                    or (item.get("location") or {}).get("range"))
            if span is not None:
                entries.append({"name": item.get("name", ""), "range": span})
            walk(item.get("children"))

    walk(result or [])
    return entries


class Session:
    """A `gti_lsp` process driven one message at a time."""

    def __init__(self, server: Path) -> None:
        self.process = subprocess.Popen(
            [str(server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.buffer = b""

    def send(self, message: dict) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(frame(message))
        self.process.stdin.flush()

    def read_message(self) -> dict:
        """Block until one complete message has been read."""
        assert self.process.stdout is not None
        while True:
            header_end = self.buffer.find(b"\r\n\r\n")
            if header_end >= 0:
                length = None
                for header in self.buffer[:header_end].decode("ascii").split("\r\n"):
                    if header.lower().startswith("content-length:"):
                        length = int(header.split(":", 1)[1].strip())
                if length is None:
                    raise RuntimeError("message has no Content-Length")
                body_start = header_end + 4
                if len(self.buffer) >= body_start + length:
                    body = self.buffer[body_start:body_start + length]
                    self.buffer = self.buffer[body_start + length:]
                    return json.loads(body)
            chunk = self.process.stdout.read(1)
            if not chunk:
                raise RuntimeError("the server closed its output stream")
            self.buffer += chunk

    def request(self, identifier: int, method: str, params: dict) -> dict:
        """Send a request and return its reply, skipping notifications."""
        self.send({"jsonrpc": "2.0", "id": identifier, "method": method,
                   "params": params})
        while True:
            message = self.read_message()
            if message.get("id") == identifier:
                return message

    def initialize(self, directory: Path) -> None:
        self.request(1, "initialize",
                     {"processId": None, "rootUri": directory.as_uri(),
                      "capabilities": {}})
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def open(self, uri: str, text: str) -> None:
        self.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                   "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                               "version": 1, "text": text}}})

    def change(self, uri: str, text: str, version: int) -> None:
        # The server advertises full sync, so the whole text travels here.
        self.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                   "params": {"textDocument": {"uri": uri, "version": version},
                              "contentChanges": [{"text": text}]}})

    def close(self) -> None:
        try:
            self.request(99, "shutdown", {})
            self.send({"jsonrpc": "2.0", "method": "exit"})
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()


def run_position_case(server: Path, directory: Path, name: str,
                      prefix: str) -> str | None:
    """Return a failure description, or None when the case passes."""
    source_path = directory / f"{name}.gti"
    text = document(prefix)
    source_path.write_text(text, encoding="utf-8")

    session = Session(server)
    try:
        session.initialize(directory)
        uri = source_path.as_uri()
        session.open(uri, text)
        reply = session.request(2, "textDocument/documentSymbol",
                                {"textDocument": {"uri": uri}})
    finally:
        session.close()

    if "result" not in reply:
        return f"no result: {json.dumps(reply)[:200]}"
    entries = [e for e in symbol_entries(reply["result"])
               if e["range"]["start"]["line"] == 0]
    if not entries:
        return "documentSymbol returned no symbols on line 0"

    line = text.split("\n")[0]
    for entry in entries:
        span, end = entry["range"]["start"], entry["range"]["end"]
        actual = slice_utf16(line, span["character"], end["character"])
        if actual != DECLARATION:
            return (f"range for {entry['name']!r} covers {actual!r}, expected "
                    f"{DECLARATION!r} (UTF-16 {span['character']}.."
                    f"{end['character']}; prefix is {len(prefix.encode())} "
                    f"bytes / {len(prefix.encode('utf-16-le')) // 2} units)")
    return None


def run_staleness(server: Path, directory: Path) -> str | None:
    """Rename the only declaration through didChange; the old name must go."""
    source_path = directory / "staleness.gti"
    before = "int32_t alpha() { return 1; }\n"
    after = "int32_t beta() { return 1; }\n"
    source_path.write_text(before, encoding="utf-8")
    uri = source_path.as_uri()

    session = Session(server)
    try:
        session.initialize(directory)
        session.open(uri, before)
        # Read the first reply before editing: didChange cancels in-flight
        # requests with -32801, so overlapping them tests nothing.
        first = session.request(2, "textDocument/documentSymbol",
                                {"textDocument": {"uri": uri}})
        session.change(uri, after, version=2)
        second = session.request(3, "textDocument/documentSymbol",
                                 {"textDocument": {"uri": uri}})
    finally:
        session.close()

    if "result" not in first or "result" not in second:
        return "a documentSymbol request did not return a result"
    names_before = {e["name"] for e in symbol_entries(first["result"])}
    names_after = {e["name"] for e in symbol_entries(second["result"])}
    if "alpha" not in names_before:
        return f"the pre-edit reply did not contain 'alpha': {sorted(names_before)}"
    if "alpha" in names_after:
        return ("'alpha' survives after being renamed away; the reply is stale: "
                f"{sorted(names_after)}")
    if "beta" not in names_after:
        return f"the post-edit reply does not contain 'beta': {sorted(names_after)}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Probe gti_lsp for position-encoding and staleness defects.")
    parser.add_argument("--server", default="build/gti_lsp")
    parser.add_argument("--check", choices=["all", "positions", "staleness"],
                        default="all")
    parser.add_argument("--keep", action="store_true",
                        help="keep the generated sources")
    arguments = parser.parse_args()

    server = (REPOSITORY_ROOT / arguments.server).resolve()
    if not server.is_file():
        print(f"error: {server} not found. The target is optional; configure "
              "with -DGTI_BUILD_LSP=ON and build gti_lsp.", file=sys.stderr)
        return 1

    directory = Path(tempfile.mkdtemp(prefix="gti-lsp-probe-"))
    failures = 0

    if arguments.check in {"all", "positions"}:
        print("position encoding (UTF-16 code units):")
        for name, prefix in CASES:
            problem = run_position_case(server, directory, name, prefix)
            if problem is None:
                print(f"  {name:<10} ok")
            else:
                failures += 1
                print(f"  {name:<10} FAILED  {problem}")

    if arguments.check in {"all", "staleness"}:
        print("edit staleness (source-unit reuse across didChange):")
        problem = run_staleness(server, directory)
        if problem is None:
            print(f"  {'rename':<10} ok")
        else:
            failures += 1
            print(f"  {'rename':<10} FAILED  {problem}")

    print()
    if failures:
        print(f"{failures} check(s) failed; sources kept in {directory}")
        return 1
    if arguments.keep:
        print(f"sources kept in {directory}")
    print("every check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
