#!/usr/bin/env python3
"""Family contracts and independent input binders retain editor identities."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = (
        "// UTF-16 provenance: \U0001f600\n"
        "concept storage<P> { typename buffer<Element>; };\n"
        "struct Heap {};\n"
        "struct Cell<T> { T value; };\n"
        "for concept storage<Heap> { using buffer<Item> = Cell<Item>; };\n"
        "using Numbers = storage<Heap>::buffer<int>;\n"
        "int main() { return 0; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-families-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params, *, expect_error=False):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            if expect_error:
                assert "error" in reply, reply
                return reply["error"]
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            reply = session.receive_until(
                lambda item: item.get("method") == "textDocument/publishDiagnostics"
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
            offset = source.rindex("buffer")
            params = at(source, offset)
            hover = request("textDocument/hover", params)
            assert "Cell<int32_t>" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(source, source.index("buffer")), definition
            references = request("textDocument/references", {
                **params, "context": {"includeDeclaration": True}})
            assert len(references) == 3, references
            # Namespace contracts preserve the existing cross-file rename guard.
            error = request("textDocument/rename", {**params, "newName": "container"},
                            expect_error=True)
            assert error["code"] == -32803 and "function-local" in error["message"], error
            binder = at(source, source.index("Cell<Item>") + 5)
            references = request("textDocument/references", {
                **binder, "context": {"includeDeclaration": True}})
            assert len(references) == 2, references
            assert all(item["range"]["start"]["line"] == 4
                       for item in references), references
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            position = params["position"]
            assert legend[tokens[(position["line"], position["character"])]["type"]] == "type", tokens

            incomplete = source.replace("::buffer<int>", "::buf")
            change(2, incomplete)
            assert diagnostics(2)
            completions = request("textDocument/completion", at(
                incomplete, incomplete.index("::buf") + 5))
            items = completions["items"] if isinstance(completions, dict) else completions
            item = next(item for item in items if item["label"] == "buffer")
            assert "buffer<Element>" in item["detail"], item

            invalid = source.replace("::buffer<int>", "::buffer<int, bool>")
            change(3, invalid)
            errors = diagnostics(3)
            error = next(error for error in errors if error.get("code") == "GTI-S2123")
            offset = invalid.rindex("buffer")
            assert error["range"] == {"start": lsp_position(invalid, offset),
                                      "end": lsp_position(invalid, offset + 6)}, error
            change(4, source.replace("buffer<Element>", "buffer<Element,"))
            assert diagnostics(4)
            change(5, source)
            assert not diagnostics(5)
            recovered = request("textDocument/definition", params)
            assert recovered == definition, recovered
            class_source = source + (
                "struct Stored { using Selected = storage<Heap>::buffer<int>; };\n")
            change(6, class_source)
            assert not diagnostics(6)
            hover = request("textDocument/hover", at(
                class_source, class_source.index("Selected")))
            assert "Cell<int32_t>" in str(hover), hover
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
