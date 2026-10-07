#!/usr/bin/env python3
"""Generic advisory fixes must remain safe for the current source generation."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: generic_advisory_lsp_test.py <gti_lsp>")
    root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="gti-generic-advisory-lsp-") as directory:
        uri = (pathlib.Path(directory) / "advisory.gti").as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {
                    "workspace": {"workspaceEdit": {"documentChanges": True}},
                    "textDocument": {
                        "publishDiagnostics": {"dataSupport": True, "tagSupport": {"valueSet": [1]}},
                        "codeAction": {"codeActionLiteralSupport": {
                            "codeActionKind": {"valueSet": ["quickfix"]}}}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

            def change(source, version):
                if version == 1:
                    session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                        "textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}})
                else:
                    session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                        "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"].get("uri") == uri and
                    message["params"].get("version") == version)["params"]["diagnostics"]

            def actions(diagnostic, request_id):
                session.send({"jsonrpc": "2.0", "id": request_id, "method": "textDocument/codeAction", "params": {
                    "textDocument": {"uri": uri}, "range": diagnostic["range"],
                    "context": {"diagnostics": [diagnostic], "only": ["quickfix"]}}})
                return session.receive_until(lambda message: message.get("id") == request_id)["result"]

            for version, name in enumerate(("a20-unnecessary-mut-per-instance",
                                             "a24-unreachable-warning-per-instance",
                                             "a33-unnecessary-mut-deref-store"), 1):
                source = "// 😀 UTF-16 control\n" + (root / "coverage/reproducers" / f"{name}.gti").read_text()
                assert change(source, version) == [], name

            source = ('T identity<T>(T value) { /* 😀 */ mut int unused = 1; return value; }\n'
                      'int main() { return identity(0); }\n')
            diagnostics = change(source, 4)
            assert len(diagnostics) == 1, diagnostics
            warning = diagnostics[0]
            offset = source.index("mut")
            assert warning["code"] == "GTI-S2096" and warning["severity"] == 2, warning
            assert warning["range"] == {"start": lsp_position(source, offset),
                                        "end": lsp_position(source, offset + 3)}, warning
            fixes = actions(warning, 2)
            assert len(fixes) == 1, fixes
            edit = fixes[0]["edit"]["documentChanges"][0]
            assert edit["textDocument"] == {"uri": uri, "version": 4}, edit
            assert edit["edits"] == [{"range": warning["range"], "newText": ""}], edit
            assert change(source[:offset] + source[offset + 3:], 5) == []
            assert not actions(warning, 3), "stale mut advice cannot edit the newer snapshot"
            reversed_instances = (root / "coverage/reproducers/a24-unreachable-warning-per-instance.gti").read_text()
            reversed_instances = reversed_instances.replace("adjust<0>(1) + adjust<1>(1)",
                                                            "adjust<1>(1) + adjust<0>(1)")
            assert change(reversed_instances, 6) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()
