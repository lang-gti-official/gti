#!/usr/bin/env python3
"""Removed include syntax reports migration errors in each editor file."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-include-removal-") as directory:
        root = pathlib.Path(directory)
        (root / "leaf.gti").write_text("export module leaf;\nexport int value() { return 1; }\n")
        source = '/* 😀 */ #include "leaf.gti"\nint main() {}\n'
        session = LspSession(sys.argv[1])
        def send(method, params):
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
        def publication(uri, version):
            return session.receive_until(lambda m:
                m.get("method") == "textDocument/publishDiagnostics" and
                m["params"].get("uri") == uri and m["params"].get("version") == version
            )["params"]["diagnostics"]
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
            session.receive_until(lambda m: m.get("id") == 1)
            send("initialized", {})
            for name in ("one.gti", "two.gti"):
                path = root / name
                path.write_text(source)
                uri = path.as_uri()
                send("textDocument/didOpen", {"textDocument": {
                    "uri": uri, "languageId": "gti", "version": 1, "text": source}})
                errors = publication(uri, 1)
                removed = [d for d in errors if d.get("code") == "GTI-I0014"]
                assert len(removed) == 1 and removed[0]["severity"] == 1, errors
                offset = source.index("#include")
                assert removed[0]["range"] == {"start": lsp_position(source, offset),
                                              "end": lsp_position(source, offset + 8)}, removed
                migrated = 'import "leaf.gti";\nint main() { return value(); }\n'
                send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
                     "contentChanges": [{"text": migrated}]})
                assert publication(uri, 2) == []
        finally:
            session.close()
    print("Include removal LSP checks passed")
if __name__ == "__main__":
    main()
