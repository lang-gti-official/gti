#!/usr/bin/env python3
"""Unexpected error ownership is diagnosed by the same CLI/LSP semantics."""

from pathlib import Path
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def run(compiler, server, root):
    source = '''#include <std/string>
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
    assert built.returncode == 65 and "GTI-S2003" in built.stderr, built
    assert "Cannot copy error payload" in built.stderr and "GTI-B0001" not in built.stderr, built
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
        error = next(d for d in errors if d.get("code") == "GTI-S2003")
        offset = source.index("error());")
        assert error["range"]["start"] == lsp_position(source, offset), error
        assert error["range"]["end"] == lsp_position(source, offset + 5), error
        assert "move the carrier" in error["message"].lower(), error
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
