#!/usr/bin/env python3
"""Discard policy comes from resolved compiler declarations, including edits."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    invalid = "int value() { return 7; }\nint main() { /*😀*/ value(); return 0; }\n"
    valid = "[[discardable]] " + invalid
    with tempfile.TemporaryDirectory(prefix="gti-discardable-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(invalid)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])

        def published(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version
            )["params"]["diagnostics"]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": source}]}})
            return published(version)

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {"dataSupport": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": invalid}}})
            errors = published(1)
            diagnostic = next(d for d in errors if d["code"] == "GTI-S2009")
            offset = invalid.rindex("value()") + len("value(")
            assert diagnostic["range"] == {"start": lsp_position(invalid, offset),
                                           "end": lsp_position(invalid, offset + 1)}, diagnostic
            assert not change(2, valid)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, valid.rindex("value()") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "[[discardable]]" in hover["contents"]["value"], hover
            assert change(3, valid.replace("[[discardable]]", "[[discardable("))
            assert not change(4, valid)
            assert any(d["code"] == "GTI-S2009" for d in change(5, invalid))
            assert not change(6, invalid.replace("/*😀*/ value();", "/*😀*/ [[discard]] value();"))
        finally:
            session.close()
    print("discardable-result LSP diagnostics, hover, unsaved edits and recovery passed")


if __name__ == "__main__":
    main()
