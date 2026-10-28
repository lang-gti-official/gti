#!/usr/bin/env python3
"""Publish owning recovery errors from current snapshots, with UTF-16 spans."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: diagnostic_recovery_lsp_test.py <gti_lsp>")
    with tempfile.TemporaryDirectory(prefix="gti-diagnostic-recovery-") as directory:
        path = pathlib.Path(directory) / "recovery.gti"
        uri = path.as_uri()
        source = "/* 😀 */ [[noretrun]] void stop() {}\nint main() { return 0; }\n"
        path.write_text(source)
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True, "dataSupport": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and
                    message["params"].get("version") == version
                )["params"]["diagnostics"]

            def change(text, version):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text}]}})
                return diagnostics(version)

            errors = diagnostics(1)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2099", errors
            offset = source.index("noretrun")
            assert errors[0]["range"] == {
                "start": lsp_position(source, offset),
                "end": lsp_position(source, offset + 8)}, errors
            stale = errors[0]
            valid = source.replace("[[noretrun]] ", "")
            assert change(valid, 2) == []
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
                "textDocument": {"uri": uri}, "range": stale["range"],
                "context": {"diagnostics": [stale], "only": ["quickfix"]}}})
            assert not session.receive_until(lambda message: message.get("id") == 2)["result"]

            malformed = "#if FLAG\nint first() { return 1; }\n#endif\nint main() { return 0; }\n"
            errors = change(malformed, 3)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-P0003", errors
            assert change(valid, 4) == []

            value = ("uint64_t extent<uint64_t N>() { return N; }\n"
                     "int main() { uint64_t runtime = 4; return int32_t(extent<runtime>()); }\n")
            errors = change(value, 5)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2026", errors
            offset = value.rindex("runtime")
            assert errors[0]["range"] == {
                "start": lsp_position(value, offset),
                "end": lsp_position(value, offset + 7)}, errors
            assert "compile-time value" in errors[0]["message"], errors
            assert change(value.replace("extent<runtime>", "extent<4>"), 6) == []

            local = "int main() { /* 😀 */ mut int32_t value; value = 4; return value; }\n"
            errors = change(local, 7)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2000", errors
            offset = local.index("value")
            assert errors[0]["range"] == {
                "start": lsp_position(local, offset),
                "end": lsp_position(local, offset + 5)}, errors
            assert "requires an initializer" in errors[0]["message"], errors
            assert "later assignment" in errors[0]["message"], errors
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/codeAction", "params": {
                "textDocument": {"uri": uri}, "range": errors[0]["range"],
                "context": {"diagnostics": errors, "only": ["quickfix"]}}})
            assert not session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert change(local.replace("value;", "value = 0;", 1), 8) == []

            designated = ("struct Point { int x; };\n"
                          "int read(Point value) { return value.x; }\n"
                          "int main() { /* 😀 */ return read({.x = 1}); }\n")
            errors = change(designated, 9)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2091", errors
            offset = designated.index("{.x")
            assert errors[0]["range"] == {
                "start": lsp_position(designated, offset),
                "end": lsp_position(designated, offset + 1)}, errors
            assert "destination struct type" in errors[0]["message"], errors
            assert "explicit struct type" in errors[0]["message"], errors
            assert "Designated construction requires" not in errors[0]["message"], errors
            session.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/codeAction", "params": {
                "textDocument": {"uri": uri}, "range": errors[0]["range"],
                "context": {"diagnostics": errors, "only": ["quickfix"]}}})
            assert not session.receive_until(lambda message: message.get("id") == 4)["result"]
            fixed = designated.replace("return read({.x = 1});",
                                       "Point point = {.x = 1}; return read(point);")
            assert change(fixed, 10) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()
