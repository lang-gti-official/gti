#!/usr/bin/env python3
"""Borrow captures share diagnostics, formatting and recovery with compilation."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """int main() {
  mut int value = 3;
  // supplementary character: 😀
  mut auto update = [mut &value]() -> int { value += 2; return value; };
  return update() - 5;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-borrow-capture-lsp-") as directory:
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
            hover = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": lsp_position(SOURCE, SOURCE.index("value +="))})
            assert hover and "int32_t" in str(hover), hover
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else SOURCE
            assert "[mut &value]" in formatted, formatted
            clean(change(2, formatted))
            errors = change(3, SOURCE.replace("[mut &value]", "[&value]"))
            assert any(item.get("code") == "GTI-S2002" for item in errors), errors
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors
            clean(change(4, SOURCE))
            assert change(5, SOURCE.replace("[mut &value]", "[mut &]"))
            clean(change(6, SOURCE))
            conflicts = SOURCE.replace("  return update()", "  value = 7;\n  return update()")
            errors = change(7, conflicts)
            assert any(item.get("code") == "GTI-S2017" for item in errors), errors
            clean(change(8, SOURCE))
        finally:
            session.close()
    print("Borrowing callback LSP checks passed")


if __name__ == "__main__":
    main()
