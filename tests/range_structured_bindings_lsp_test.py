#!/usr/bin/env python3
"""Range patterns retain diagnostics, symbol navigation, formatting and recovery."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """struct Entry { mut int key; mut int value; Entry(int k, int v) : key(k), value(v) {} };
int main() {
  mut Entry entries[1] = {Entry(1, 2)};
  /* 😀 */ for (auto& [key, value] : entries) { int result = key + value; }
  for (auto [key, value] : entries) { int result = key + value; }
  for (mut auto& [key, value] : entries) { key += 1; value += key; }
  return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-range-pattern-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def publication(version):
            result = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)
            return result["params"]["diagnostics"]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": source}]}})
            return publication(version)

        def clean(diagnostics):
            assert not [item for item in diagnostics if item.get("severity", 1) == 1], diagnostics

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": SOURCE}}})
            clean(publication(1))
            position = lsp_position(SOURCE, SOURCE.index("key + value"))
            hover = request("textDocument/hover", {
                "textDocument": {"uri": uri}, "position": position})
            assert hover and "int32_t" in str(hover), hover
            definition = request("textDocument/definition", {
                "textDocument": {"uri": uri}, "position": position})
            assert definition, definition
            locations = definition if isinstance(definition, list) else [definition]
            assert any(item.get("range", item.get("targetSelectionRange", {})).get("start") ==
                       lsp_position(SOURCE, SOURCE.index("key, value]"))
                       for item in locations), definition
            rename = request("textDocument/rename", {
                "textDocument": {"uri": uri}, "position": position, "newName": "entry_key"})
            assert rename, rename
            edits = rename.get("changes", {}).get(uri, [])
            if not edits:
                edits = [edit for change in rename.get("documentChanges", [])
                         for edit in change.get("edits", [])]
            assert len(edits) == 2, rename
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else SOURCE
            assert "auto& [key, value] : entries" in formatted, formatted
            assert "auto [key, value] : entries" in formatted, formatted
            assert "mut auto& [key, value] : entries" in formatted, formatted
            clean(change(2, formatted))
            errors = change(3, SOURCE.replace("[key, value]", "[key]", 1))
            assert errors and not any(item.get("code") == "GTI-B0001" for item in errors), errors
            clean(change(4, SOURCE))
            errors = change(5, SOURCE.replace("[key, value]", "[key, ]", 1))
            assert errors and not any(item.get("code") == "GTI-B0001" for item in errors), errors
            clean(change(6, SOURCE))
        finally:
            session.close()
    print("Range structured binding LSP checks passed")


if __name__ == "__main__":
    main()
