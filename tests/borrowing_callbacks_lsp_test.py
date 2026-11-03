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

REFERENCE_SOURCE = """int main() {
  // supplementary character: 😀
  auto increment = [](mut int& value) -> mut int& {
    value += 1;
    return value;
  };
  auto relay = [](mut int& value) -> mut int& {
    mut int& alias = value;
    return alias;
  };
  auto observe = [](mut int& value) -> int& {
    value += 1;
    return value;
  };
  mut int number = 10;
  mut int& changed = increment(relay(number));
  changed += 5;
  int& observed = observe(number);
  return observed - 17;
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
            clean(change(9, REFERENCE_SOURCE))
            hover = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": lsp_position(
                    REFERENCE_SOURCE, REFERENCE_SOURCE.index("changed +="))})
            assert hover and "int32_t" in str(hover), hover
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else REFERENCE_SOURCE
            assert "-> mut int&" in formatted, formatted
            clean(change(10, formatted))
            conflicts = REFERENCE_SOURCE.replace(
                "  return observed", "  number += 1;\n  return observed")
            errors = change(11, conflicts)
            assert any(item.get("code") == "GTI-S2017" for item in errors), errors
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors
            clean(change(12, REFERENCE_SOURCE))
            assert change(13, REFERENCE_SOURCE.replace("-> mut int&", "-> mut"))
            clean(change(14, REFERENCE_SOURCE))
        finally:
            session.close()
    print("Borrowing callback LSP checks passed")


if __name__ == "__main__":
    main()
