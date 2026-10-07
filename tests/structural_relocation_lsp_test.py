#!/usr/bin/env python3
"""Relocation concepts use the shared semantic/editor constraint contract."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = """class Item { public: Item() {} };
void accept<std::bitwise_relocatable T>() {}
int main() { /*😀*/ accept<Item>(); return 0; }
"""
    with tempfile.TemporaryDirectory(prefix="gti-relocation-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            reply = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)
            return reply["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            assert not diagnostics(1)
            concept = {"textDocument": {"uri": uri},
                       "position": lsp_position(source, source.index("bitwise_relocatable") + 2)}
            hover = request("textDocument/hover", concept)
            assert "concept std::bitwise_relocatable" in str(hover), hover
            definition = request("textDocument/definition", concept)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"].endswith("/stdlib/prelude.gti"), definition

            invalid = source.replace("Item() {}", "Item() {} ~Item() {}")
            change(2, invalid)
            errors = diagnostics(2)
            error = next(item for item in errors if item.get("code") == "GTI-S2029")
            assert "bitwise_relocatable" in error["message"], error
            assert error["range"]["start"]["line"] == 2, error
            assert error.get("relatedInformation") and error.get("data", {}).get("hints"), error

            # Declared cleanup denies bytewise eligibility, not relocation.
            change(3, invalid.replace("bitwise_relocatable", "relocatable"))
            assert not diagnostics(3)
            change(4, source.replace("accept<Item>()", "accept<"))
            assert diagnostics(4)
            change(5, source)
            assert not diagnostics(5)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            change(6, formatted)
            assert not diagnostics(6)
            print("structural relocation LSP: passed")
        finally:
            session.close()


if __name__ == "__main__":
    main()
