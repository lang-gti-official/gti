#!/usr/bin/env python3
"""Runtime layout uses normal generic navigation and shared diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    valid = ("#include <std/memory>\nclass Pinned {\npublic:\n"
             "  Pinned(Pinned& other) = delete;\n"
             "  Pinned(Pinned&& other) = delete;\n};\n"
             "int main() { auto descriptor = std::layout_of<Pinned>(); return 0; }\n")
    with tempfile.TemporaryDirectory(prefix="gti-layout-lsp-") as directory:
        path = pathlib.Path(directory) / "layout.gti"
        path.write_text(valid)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True, "dataSupport": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": valid}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            assert not diagnostics(1)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(valid, valid.index("layout_of") + 2)}})
            definition = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert definition and "memory.gti" in str(definition), definition
            invalid = valid.replace("layout_of<Pinned>", "layout_of<void>")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": invalid}]}})
            errors = diagnostics(2)
            error = next(error for error in errors if error.get("code") == "GTI-S2029")
            assert "layout_queryable" in error["message"], error
            # Shared generic diagnostics anchor a zero-runtime-argument call
            # at its closing parenthesis and relate the exact requirement.
            offset = invalid.index("(); return") + 1
            assert error["range"] == {"start": lsp_position(invalid, offset),
                                      "end": lsp_position(invalid, offset + 1)}, error
            assert any("layout_queryable<void>" in item["message"]
                       for item in error["relatedInformation"]), error
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": valid}]}})
            assert not diagnostics(3)
        finally:
            session.close()
    print("runtime object layout LSP navigation and diagnostics passed")


if __name__ == "__main__":
    main()
