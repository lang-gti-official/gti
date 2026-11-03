#!/usr/bin/env python3
"""Concept tests retain compiler-owned identities, diagnostics, and formatting."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = (
        "concept sized<T> = requires(T& v) { { v.size() } -> uint64_t; };\n"
        "struct Box { Box() = default; uint64_t size() { return 3; } };\n"
        "uint64_t measure<T>(T& value) { if constexpr(sized<T>) "
        "{ return value.size(); } else { return 0; } }\n"
        "int main() { Box box{}; bool value = /* 😀 */ sized<Box>; "
        "return int(measure(box)); }\n"
        "static_assert(sized<Box> && !sized<int> || std::integral<int>);\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-concept-conditions-lsp-") as directory:
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

        def at(text, offset):
            return {"textDocument": {"uri": uri}, "position": lsp_position(text, offset)}

        try:
            capabilities = request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            legend = capabilities["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            assert not diagnostics(1)
            offset = source.index("sized<Box>")
            params = at(source, offset)
            hover = request("textDocument/hover", params)
            assert "concept sized" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(source, source.index("sized")), definition
            references = request("textDocument/references", {**params, "context": {"includeDeclaration": True}})
            assert len(references) == 5, references
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            pos = params["position"]
            assert legend[tokens[(pos["line"], pos["character"])]["type"]] == "type", tokens
            type_params = at(source, offset + len("sized<"))
            type_definition = request("textDocument/definition", type_params)
            type_location = type_definition[0] if isinstance(type_definition, list) else type_definition
            assert type_location["range"]["start"] == lsp_position(source, source.index("Box")), type_definition

            invalid = source[:offset] + source[offset:].replace("sized<Box>", "sized<Box, int>", 1)
            change(2, invalid)
            error = next(item for item in diagnostics(2) if item.get("code") == "GTI-S2049")
            assert error["range"] == {"start": lsp_position(invalid, offset),
                                      "end": lsp_position(invalid, offset + len("sized"))}, error
            assert "requires 1 type argument" in error["message"], error
            change(3, source.replace("if constexpr(sized<T>)", "if constexpr(sized<)"))
            assert diagnostics(3)
            change(4, source)
            assert not diagnostics(4)
            assert request("textDocument/definition", params) == definition
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            assert "sized<Box> && !sized<int> || std::integral<int>" in formatted, formatted
            change(5, formatted)
            assert not diagnostics(5)
            edits_again = request("textDocument/formatting", {
                "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}})
            assert not edits_again or edits_again[0]["newText"] == formatted
        finally:
            session.close()
    print("concept conditions LSP: identities, tokens, UTF-16 blame, recovery and formatting passed")


if __name__ == "__main__":
    main()
