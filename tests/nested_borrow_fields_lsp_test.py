#!/usr/bin/env python3
"""Nested dependencies use compiler-owned field identities and diagnostics."""

import os
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, test_missing_standard_library_include_code_action


def main():
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    source = """import std.utility;
class View {
public:
  int& item;
  View(int& item) : item(item) {}
};
int main() {
  mut int owner = 41; owner = 42;
  auto result = std::pair<View, bool>(View(owner), true);
  // edit here
  return result.first.item - 42;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-nested-borrow-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def open_document(version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": version, "text": source}}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(1)
            initial = diagnostics(1)
            assert not initial, initial
            params = {"textDocument": {"uri": uri},
                      "position": {"line": 10, "character": source.splitlines()[10].index("item")}}
            hover = request("textDocument/hover", params)
            assert "int32_t" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"] == {
                "line": 3, "character": 7}, definition
            bad_line = "  /*😀*/ owner = 7;"
            change(2, source.replace("  // edit here", bad_line))
            errors = diagnostics(2)
            conflict = next((d for d in errors if d.get("code") == "GTI-S2017"), None)
            assert conflict is not None, errors
            column = len(bad_line[:bad_line.index("owner")].encode("utf-16-le")) // 2
            assert conflict["range"]["start"] == {"line": 9, "character": column}, conflict
            assert conflict.get("relatedInformation"), conflict
            change(3, source.replace("result.first.item", "result.first."))
            assert diagnostics(3)
            change(4, source)
            assert not diagnostics(4)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            open_document(5)
            assert not diagnostics(5)
        finally:
            session.close()
        for symbol in ("map", "unordered_map"):
            test_missing_standard_library_include_code_action(
                sys.argv[1], pathlib.Path(root), symbol=symbol,
                argument="int, int", header=f"std/{symbol}")


if __name__ == "__main__":
    main()
