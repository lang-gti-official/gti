#!/usr/bin/env python3
"""Designators resolve to the actual field for hover, definition and references."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession


def main():
    source = "struct Point { int x = 0; int y = 0; };\nint main() { Point p = {.x = 3}; return p.x; }\n"
    with tempfile.TemporaryDirectory(prefix="gti-designated-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        try:
            request("initialize", {"capabilities": {}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            publication = session.receive_until(lambda message: message.get("method") == "textDocument/publishDiagnostics")
            assert not publication["params"]["diagnostics"], publication
            params = {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 25}}
            hover = request("textDocument/hover", params)
            assert hover and "x" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location and location["range"]["start"] == {"line": 0, "character": 19}, definition
            references = request("textDocument/references", {**params, "context": {"includeDeclaration": True}})
            assert len(references) == 3, references
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": source.replace(".x = 3", ".missing = 3")}]}})
            publication = session.receive_until(lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 2)
            assert any(d.get("code") == "GTI-S2091" for d in publication["params"]["diagnostics"]), publication
        finally:
            session.close()


if __name__ == "__main__":
    main()
