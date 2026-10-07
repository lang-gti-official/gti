#!/usr/bin/env python3
"""Member contracts share frontend diagnostics, signatures and formatting."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession


def main():
    source = (
        'concept snapshots<T> = requires(T& item) { { item.snapshot() } -> T; };\n'
        'struct Item { Item snapshot() { Item result{}; return result; } };\n'
        'T copy<T>(T& item) requires snapshots<T> { return item.snapshot(); }\n'
        'int main() { Item item{}; auto result = copy(item); return 0; }\n')
    with tempfile.TemporaryDirectory(prefix="gti-member-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            reply = session.receive_until(lambda m: m.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                and m["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            assert not diagnostics(1)
            hover = request("textDocument/hover", {"textDocument": {"uri": uri},
                "position": {"line": 2, "character": source.splitlines()[2].index("snapshots")}})
            assert "snapshot()" in str(hover) and "requires" in str(hover), hover
            receiver = source.splitlines()[0].index("item.snapshot")
            definition = request("textDocument/definition", {"textDocument": {"uri": uri},
                "position": {"line": 0, "character": receiver}})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == {
                "line": 0, "character": source.splitlines()[0].index("item)")}, definition
            change(2, source.replace("Item snapshot()", "Item snapshot() mut"))
            errors = diagnostics(2)
            assert any(d.get("code") == "GTI-S2029" for d in errors), errors
            change(3, source.replace("item.snapshot() }", "item.snapshot( }"))
            assert any(d.get("code") == "GTI-P0001" for d in diagnostics(3))
            change(4, source)
            assert not diagnostics(4)
            params = {"textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}}
            edits = request("textDocument/formatting", params)
            formatted = edits[0]["newText"] if edits else source
            change(5, formatted)
            assert not diagnostics(5)
            repeated = request("textDocument/formatting", params)
            assert not repeated or repeated[0]["newText"] == formatted, repeated
        finally:
            session.close()


if __name__ == "__main__":
    main()
