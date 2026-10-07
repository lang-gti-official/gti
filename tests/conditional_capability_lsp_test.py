#!/usr/bin/env python3
"""Conditional capability binders and diagnostics come from shared semantics."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position


def main():
    source = ("// 😀 UTF-16 control\n"
              "[[unsafe_transfer(T), unsafe_share(T)]] class Owner<T> { T* pointer = nullptr; };\n"
              "int main() { return 0; }\n")
    with tempfile.TemporaryDirectory(prefix="gti-conditional-lsp-") as directory:
        path = pathlib.Path(directory) / "owner.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            assert not diagnostics(1)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(source, source.index("unsafe_transfer(T)") + 16)}})
            result = session.receive_until(lambda message: message.get("id") == 2)["result"]
            target = source.index("Owner<T>") + 6
            assert result == {"uri": uri, "range": {
                "start": lsp_position(source, target), "end": lsp_position(source, target + 1)}}, result
            invalid = source.replace("unsafe_share(T)", "unsafe_share(U)")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": invalid}]}})
            error = next(d for d in diagnostics(2) if d["code"] == "GTI-S2059")
            offset = invalid.index("unsafe_share(U)") + 13
            assert "non-pack type parameter" in error["message"], error
            assert error["range"] == {"start": lsp_position(invalid, offset),
                                      "end": lsp_position(invalid, offset + 1)}, error
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": source}]}})
            assert not diagnostics(3)
        finally:
            session.close()
    print("conditional capability LSP binder navigation and versioned diagnostics passed")


if __name__ == "__main__":
    main()
