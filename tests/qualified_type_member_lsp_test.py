#!/usr/bin/env python3
"""Specialized type members retain compiler-owned identities in the editor."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = (
        "// UTF-16 provenance: \U0001f600\n"
        "struct Box<T> { using value_type = T; };\n"
        "struct Outer<T> { using inner = Box<T>; };\n"
        "using Value = Outer<int>::inner::value_type;\n"
        "int main() { Value item = 7; return item - 7; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-qualified-members-lsp-") as directory:
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

        def diagnostics(version):
            reply = session.receive_until(
                lambda item: item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version
            )
            return reply["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            capabilities = request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}
            }})
            legend = capabilities["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {
                              "uri": uri, "languageId": "gti", "version": 1,
                              "text": source}}})
            assert not diagnostics(1)
            alias_offset = source.rindex("value_type")
            position = lsp_position(source, alias_offset)
            params = {"textDocument": {"uri": uri}, "position": position}
            hover = request("textDocument/hover", params)
            assert "value_type" in str(hover) and "int32_t" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(source, source.index("value_type")), definition
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            assert legend[tokens[(position["line"], position["character"])]["type"]] == "type", tokens
            references = request("textDocument/references", {
                **params, "context": {"includeDeclaration": True}})
            assert len(references) == 2, references

            # A specialized type owner supplies substituted completion details.
            completion_source = source.replace("Outer<int>::inner::value_type", "Box<uint64_t>::val")
            change(2, completion_source)
            assert diagnostics(2)
            completions = request("textDocument/completion", {
                "textDocument": {"uri": uri},
                "position": lsp_position(completion_source, completion_source.index("::val") + 5)})
            items = completions["items"] if isinstance(completions, dict) else completions
            item = next(item for item in items if item["label"] == "value_type")
            assert "Box<uint64_t>::value_type = uint64_t" in item["detail"], item

            missing = source.replace("inner::value_type", "inner::missing")
            change(3, missing)
            errors = diagnostics(3)
            error = next(error for error in errors if error.get("code") == "GTI-S2030")
            start = missing.index("missing")
            assert "no direct type alias" in error["message"], error
            assert error["range"] == {"start": lsp_position(missing, start),
                                      "end": lsp_position(missing, start + 7)}, error
            change(4, source.replace("inner::value_type", "inner::"))
            assert diagnostics(4)
            change(5, source)
            assert not diagnostics(5)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            assert "Outer<int>::inner::value_type" in formatted, formatted
            change(6, formatted)
            assert not diagnostics(6)

            private = source.replace("struct Box<T> {", "class Box<T> {")
            change(7, private)
            assert any(error.get("code") == "GTI-S2007" for error in diagnostics(7))
            change(8, source)
            assert not diagnostics(8)

            # The same spelling works with source-defined public stdlib aliases.
            vector_source = (
                "import std.vector;\n"
                "using Cursor = std::vector<int>::const_iterator;\n"
                "using Writable = std::vector<int>::iterator;\n"
                "using End = std::vector<int>::sentinel;\n"
                "int main() { return 0; }\n"
            )
            change(9, vector_source)
            assert not diagnostics(9)
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
