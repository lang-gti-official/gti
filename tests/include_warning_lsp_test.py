#!/usr/bin/env python3
"""Per-file include advice follows current editor snapshots."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-include-warning-") as directory:
        root = pathlib.Path(directory)
        (root / "leaf.gti").write_text("")
        original = '// #include "comment.gti"\n/* 😀 */ #include "leaf.gti"\n#include "leaf.gti"\nint main() {}\n'
        session = LspSession(sys.argv[1])
        def send(method, params):
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
        def diagnostics(uri, version):
            return session.receive_until(lambda m:
                m.get("method") == "textDocument/publishDiagnostics" and
                m["params"].get("uri") == uri and
                m["params"].get("version") == version)["params"]["diagnostics"]
        def check(uri, version, text):
            items = diagnostics(uri, version)
            assert not [d for d in items if d.get("severity", 1) == 1], items
            warnings = [d for d in items if d.get("code") == "GTI-I0013"]
            offset = text.find('#include "leaf.gti"')
            assert len(warnings) == (0 if offset < 0 else 1), items
            if warnings:
                warning = warnings[0]
                assert warning["severity"] == 2, warning
                assert "Prefer module imports" in warning["message"], warning
                assert warning["range"] == {"start": lsp_position(text, offset),
                                            "end": lsp_position(text, offset + 8)}, warning
        def change(uri, version, text):
            send("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                 "contentChanges": [{"text": text}]})
            check(uri, version, text)
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda m: m.get("id") == 1)
            send("initialized", {})
            for name in ("first.gti", "second.gti"):
                path = root / name
                path.write_text(original)
                uri = path.as_uri()
                send("textDocument/didOpen", {"textDocument": {
                    "uri": uri, "languageId": "gti", "version": 1, "text": original}})
                check(uri, 1, original)
            uri = (root / "first.gti").as_uri()
            moved = original.replace('/* 😀 */ #include "leaf.gti"\n', '')
            change(uri, 2, moved)
            change(uri, 3, "int main() {}\n")
            change(uri, 4, original)
            send("textDocument/didClose", {"textDocument": {"uri": uri}})
            send("textDocument/didOpen", {"textDocument": {
                "uri": uri, "languageId": "gti", "version": 5, "text": original}})
            check(uri, 5, original)
        finally:
            session.close()
    print("Per-file include LSP checks passed")

if __name__ == "__main__":
    main()
