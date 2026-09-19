#!/usr/bin/env python3
"""The shared frontend owns unsafe-call diagnostics and visible signatures."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unsafe_function_lsp_test.py <gti_lsp>")
    source = "// 😀 UTF-16 control\n[[unsafe]] int32_t raw() { return 1; }\nint main() { return raw(); }\n"
    with tempfile.TemporaryDirectory(prefix="gti-unsafe-lsp-") as directory:
        path = pathlib.Path(directory) / "unsafe.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            errors = diagnostics(1)
            error = next(d for d in errors if d["code"] == "GTI-S2055")
            closing = source.rindex("raw()") + 4
            assert error["range"] == {"start": lsp_position(source, closing),
                                      "end": lsp_position(source, closing + 1)}, error
            related = error["relatedInformation"][0]["location"]
            attribute = source.index("unsafe")
            assert related == {"uri": uri, "range": {
                "start": lsp_position(source, attribute),
                "end": lsp_position(source, attribute + 6)}}, error
            valid = source.replace("return raw();", "unsafe { return raw(); }")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert diagnostics(2) == []
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, valid.rindex("raw()") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "[[unsafe]]" in hover["contents"]["value"], hover
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/codeAction", "params": {
                "textDocument": {"uri": uri}, "range": error["range"], "context": {"diagnostics": [error]}}})
            actions = session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert not actions, actions
        finally:
            session.close()


if __name__ == "__main__":
    main()
