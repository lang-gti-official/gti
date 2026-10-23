#!/usr/bin/env python3
"""Deferred helpers use compiler diagnostics, signatures and current overlays."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = ("#include <std/assert>\n"
              "int main() { /*😀*/ std::assert(1); return 0; }\n")
    valid = source.replace("assert(1)", "assert(true)")
    with tempfile.TemporaryDirectory(prefix="gti-deferred-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])

        def published(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version
            )["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}]}})
            return published(version)

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            diagnostics = published(1)
            error = next(item for item in diagnostics if item["code"] == "GTI-S2012")
            assert error["range"]["start"]["line"] == 1, error
            assert error["range"]["start"]["character"] >= lsp_position(source, source.index("std::assert"))["character"], error
            assert not change(2, valid)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(valid, valid.index("assert(true)") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "[[deferred]]" in hover["contents"]["value"], hover
            malformed = "[[deferred]] int bad() { return 1; }\n" + valid
            assert any(item["code"] == "GTI-S2099" for item in change(3, malformed))
            assert not change(4, valid)
            incomplete = valid.replace("std::assert(true)", "std::assert(")
            assert change(5, incomplete)
            assert not change(6, valid)
        finally:
            session.close()
    print("deferred assertion LSP diagnostics, UTF-16, hover and overlay recovery passed")


if __name__ == "__main__":
    main()
