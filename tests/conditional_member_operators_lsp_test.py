#!/usr/bin/env python3
"""Conditional operators retain compiler-owned signatures and diagnostics."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position

SOURCE = """concept available<T> = std::numeric<T>;
class Box<T> {
public:
  Box() = default;
  int operator[](int index) requires available<T> { return index; }
  int operator()(int value) requires available<T> { return value; }
};
int main() { /* 🧮 */ Box<int> box{}; return box[0] + box(0); }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-conditional-operator-lsp-") as directory:
        uri = (pathlib.Path(directory) / "operators.gti").as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def change(source, version):
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            return session.receive_until(lambda item: item.get("method") == "textDocument/publishDiagnostics" and item["params"].get("uri") == uri and item["params"].get("version") == version)["params"]["diagnostics"]

        try:
            request("initialize", {"capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert change(SOURCE, 1) == []
            position = lsp_position(SOURCE, SOURCE.index("available<T> {"))
            definition = request("textDocument/definition", {"textDocument": {"uri": uri}, "position": position})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == lsp_position(SOURCE, SOURCE.index("available<T>")), definition
            hover = request("textDocument/hover", {"textDocument": {"uri": uri}, "position": lsp_position(SOURCE, SOURCE.index("operator[]") + len("operator"))})
            assert "available<T>" in str(hover), hover
            tokens = request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
            assert tokens["data"], tokens
            invalid = SOURCE.replace("Box<int> box", "Box<bool> box")
            diagnostics = change(invalid, 2)
            errors = [entry for entry in diagnostics if entry.get("code") == "GTI-S2029"]
            assert errors and any(entry.get("relatedInformation") for entry in errors), diagnostics
            assert any(entry["range"]["start"] == lsp_position(invalid, invalid.index("[0]")) for entry in errors), errors
            assert not any(entry.get("code") == "GTI-B0001" for entry in diagnostics), diagnostics
            broken = SOURCE.replace("available<T> {", "available< {", 1)
            assert change(broken, 3)
            assert change(SOURCE, 4) == []
            edits = request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}})
            if edits:
                formatted = edits[0]["newText"]
                assert "requires available<T>" in formatted, formatted
                assert change(formatted, 5) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()
