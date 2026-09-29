#!/usr/bin/env python3
"""Payload reference bindings retain compiler-owned navigation and diagnostics."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, open_document


def main():
    source = """enum class Data { some(int32_t item), none };
int main() {
  mut Data input = Data::some(3);
  switch (input) {
    case Data::some(mut auto& item): item = 7; return item;
    case Data::none: return 0;
  }
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-pattern-lsp-") as temporary:
        uri = (pathlib.Path(temporary) / "pattern.gti").as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(session, uri, source)
            use = source.index("item;", source.index("case Data::some"))
            declaration = source.index("item):")
            for request_id, method in ((2, "textDocument/hover"),
                                       (3, "textDocument/definition"),
                                       (4, "textDocument/references")):
                params = {"textDocument": {"uri": uri},
                          "position": lsp_position(source, use)}
                if request_id == 4:
                    params["context"] = {"includeDeclaration": True}
                session.send({"jsonrpc": "2.0", "id": request_id,
                              "method": method, "params": params})
                result = session.receive_until(
                    lambda message: message.get("id") == request_id)["result"]
                assert result, (method, result)
                if request_id == 2:
                    assert "int32_t" in str(result), result
                elif request_id == 3:
                    location = result[0] if isinstance(result, list) else result
                    assert location["range"]["start"] == lsp_position(source, declaration), result
                else:
                    assert len(result) >= 3, result
            session.send({"jsonrpc": "2.0", "id": 5,
                          "method": "textDocument/semanticTokens/full",
                          "params": {"textDocument": {"uri": uri}}})
            tokens = session.receive_until(lambda message: message.get("id") == 5)["result"]["data"]
            line = column = 0
            positions = set()
            for index in range(0, len(tokens), 5):
                delta_line, delta_column, _, _, _ = tokens[index:index + 5]
                line += delta_line
                column = delta_column if delta_line else column + delta_column
                positions.add((line, column))
            declaration_position = lsp_position(source, declaration)
            assert (declaration_position["line"], declaration_position["character"]) in positions
            invalid = source.replace("mut Data input", "Data input")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": invalid}]}})
            diagnostics = session.receive_until(
                lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 2)["params"]["diagnostics"]
            assert any(item.get("code") == "GTI-S2067" and
                       "mutable enum" in item["message"] for item in diagnostics), diagnostics
            diagnostic = next(item for item in diagnostics if item.get("code") == "GTI-S2067")
            assert diagnostic["range"]["start"] == lsp_position(invalid, invalid.index("item):")), diagnostic
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 3},
                                     "contentChanges": [{"text": source}]}})
            recovered = session.receive_until(
                lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 3)["params"]["diagnostics"]
            assert recovered == [], recovered
        finally:
            session.close()
    print("Borrowed payload pattern LSP checks passed")


if __name__ == "__main__":
    main()
