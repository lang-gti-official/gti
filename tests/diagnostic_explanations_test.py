#!/usr/bin/env python3
"""Compiler-owned what/why/where/options survive terminal and LSP presentation."""

import pathlib
import re
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position
from harness_timeouts import scaled


def run(compiler, server, root):
    path = root / "explanations.gti"
    uri = path.as_uri()
    source = '''#include <std/string>
int main() {
  std::string owner = std::string("hello");
  /* 借 é */ std::string destination = std::move(owner);
  /* café 😀 */ return int(owner.size());
}
'''
    path.write_text(source)

    def terminal(text, color="never"):
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text)
        result = subprocess.run([compiler, str(path), "--emit-cpp", f"--color={color}"],
                                cwd=root, text=True,
                                capture_output=True, timeout=scaled(30))
        assert result.returncode == 65, result
        return result.stderr

    plain = terminal(source)
    assert "error[GTI-S2018]" in plain and "Ownership transferred here." in plain, plain
    assert "help: Use the value before transferring it" in plain, plain
    assert "\x1b" not in plain
    # Emoji occupies two terminal cells; neither UTF-8 byte count nor UTF-16
    # offsets are the terminal's column authority. The diagnostic header keeps
    # the compiler's one-based scalar column contract.
    lines = plain.splitlines()
    excerpt = next(i for i, line in enumerate(lines) if "|   /* café 😀 */" in line)
    displayed = lines[excerpt].split("| ", 1)[1]
    marker = lines[excerpt + 1].split("| ", 1)[1]
    assert marker.index("^") == displayed.index("owner") + 1, plain
    assert f":5:{source.splitlines()[4].index('owner') + 1}: error[GTI-S2018]" in plain, plain
    colored = terminal(source, "always")
    assert re.sub(r"\x1b\[[0-9;]*m", "", colored) == plain
    tabbed = terminal(source.replace("  /* café 😀 */", "\t/* café 😀 */"))
    assert "\t" not in tabbed and "|     /* café 😀 */" in tabbed, tabbed
    controlled = terminal(source.replace("café 😀", "café\x1b[31m"))
    assert "\x1b" not in controlled and "\\x1b[31m" in controlled, controlled
    for spelling, correction in (("cafe\u0301", -1), ("界", 1)):
        output = terminal(source.replace("café 😀", spelling))
        excerpt = next(i for i, line in enumerate(output.splitlines())
                       if "owner.size()" in line)
        displayed = output.splitlines()[excerpt].split("| ", 1)[1]
        marker = output.splitlines()[excerpt + 1].split("| ", 1)[1]
        assert marker.index("^") == displayed.index("owner") + correction, output
    invalid = terminal(b"/* a\x80 */\nint main() { return 0; }\n")
    assert "error[GTI-L0013]" in invalid and "/* a\\x80 */" in invalid, invalid
    invalid_lines = invalid.splitlines()
    excerpt = next(i for i, line in enumerate(invalid_lines) if "/* a\\x80 */" in line)
    displayed = invalid_lines[excerpt].split("| ", 1)[1]
    marker = invalid_lines[excerpt + 1].split("| ", 1)[1]
    assert marker.index("^") == displayed.index("\\x80") and "^~~~" in marker, invalid
    path.write_text(source)

    session = LspSession(server)
    try:
        session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"capabilities": {"textDocument": {
                          "publishDiagnostics": {"relatedInformation": True, "dataSupport": True},
                          "codeAction": {"codeActionLiteralSupport": {
                              "codeActionKind": {"valueSet": ["quickfix"]}}}}}}})
        session.receive_until(lambda message: message.get("id") == 1)
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

        def publication(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        diagnostics = publication(1)
        error = next(d for d in diagnostics if d.get("code") == "GTI-S2018")
        assert error["range"]["start"] == lsp_position(source, source.rindex("owner")), error
        assert error["range"]["end"] == lsp_position(source, source.rindex("owner") + 5), error
        note = next(n for n in error["relatedInformation"]
                    if n["message"] == "Ownership transferred here.")
        assert note["location"]["uri"] == uri
        assert note["location"]["range"]["start"] == lsp_position(source, source.index("owner);"))
        assert note["location"]["range"]["end"] == lsp_position(source, source.index("owner);") + 5)
        assert "Use the value before transferring it" in error["message"], error
        assert "\x1b" not in str(error)
        assert not error.get("data", {}).get("fixes"), error

        fixed = source.replace("std::move(owner)", "owner.clone()")
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": fixed}]}})
        assert not publication(2), "corrected overlay must clear the ownership error"
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri}, "range": error["range"],
            "context": {"diagnostics": [error], "only": ["quickfix"]}}})
        assert session.receive_until(lambda message: message.get("id") == 2)["result"] == []

        readonly = '''struct Command { mut int name = 0; };
int main() { Command cmd{}; cmd.name = 1; return 0; }
'''
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": readonly}]}})
        diagnostic = next(d for d in publication(3) if "read-only receiver" in d["message"])
        assert "Declare 'cmd' with 'mut'" in diagnostic["message"]
        assert diagnostic["relatedInformation"][0]["location"]["range"]["start"] == lsp_position(
            readonly, readonly.index("cmd{}"))
        assert not diagnostic.get("data", {}).get("fixes")
    finally:
        session.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gti-diagnostic-explanations-") as directory:
        run(str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve()),
            pathlib.Path(directory).resolve())
    print("Terminal and LSP diagnostic explanations passed")
