#!/usr/bin/env python3
"""Unexpected error ownership is diagnosed by the same CLI/LSP semantics."""

from pathlib import Path
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def run(compiler, server, root):
    source = '''import std.string;
struct Error {
  std::string text;
  Error() : text(std::string("error")) {}
  Error(Error& other) = delete;
};
expected<int, Error> failure() { return unexpected(Error()); }
expected<int, Error> relay() {
  auto carrier = failure();
  /* café 😀 */ return unexpected(carrier.error());
}
int main() { auto result = relay(); return result ? 1 : 0; }
'''
    path = root / "unexpected.gti"
    path.write_text(source)
    built = subprocess.run([compiler, str(path), "--emit-cpp", "--color=never",
                            "-o", str(root / "rejected.cpp")],
                           capture_output=True, text=True, timeout=30)
    # unexpected(e) is an ordinary source function: copying a move-only payload
    # into its by-value parameter is the general deleted-copy error.
    assert built.returncode == 65 and "GTI-S2018" in built.stderr, built
    assert "would call the deleted copy constructor of 'Error'" in built.stderr, built
    assert "GTI-B0001" not in built.stderr, built
    assert not (root / "rejected.cpp").exists()
    uri = path.as_uri()
    session = LspSession(server)
    try:
        session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "capabilities": {"textDocument": {"publishDiagnostics": {"dataSupport": True}}}}})
        session.receive_until(lambda message: message.get("id") == 1)
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

        def publication(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        errors = publication(1)
        error = next(d for d in errors if d.get("code") == "GTI-S2018")
        argument = source.index("carrier.error()")
        close = source.index(");", argument)
        start = error["range"]["start"]
        end = error["range"]["end"]
        assert start["line"] == lsp_position(source, argument)["line"], error
        assert lsp_position(source, argument)["character"] <= start["character"], error
        assert end["character"] <= lsp_position(source, close)["character"], error
        assert "deleted copy constructor of 'Error'" in error["message"], error
        assert any("std::move" in hint for hint in error["data"]["hints"]), error
        assert not error.get("data", {}).get("fixes"), error
        fixed = source.replace("unexpected(carrier.error())", "unexpected(std::move(carrier).error())")
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": fixed}]}})
        assert not publication(2), "explicit movement must clear the diagnostic"
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri}, "range": error["range"],
            "context": {"diagnostics": [error], "only": ["quickfix"]}}})
        assert session.receive_until(lambda message: message.get("id") == 2)["result"] == []
    finally:
        session.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gti-unexpected-diagnostic-") as directory:
        run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()), Path(directory))
    print("Unexpected payload CLI/LSP diagnostics passed")
