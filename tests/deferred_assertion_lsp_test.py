#!/usr/bin/env python3
"""Typed macros use compiler diagnostics, signatures and current overlays."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = ("#include <std/assert>\n"
              "int main() { /*😀*/ std::assert(1); return 0; }\n")
    valid = source.replace("assert(1)", "assert(true)")
    with tempfile.TemporaryDirectory(prefix="gti-deferred-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])

        def published(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version
            )["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}]}})
            return published(version)

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            initialized = session.receive_until(lambda message: message.get("id") == 1)["result"]
            token_types = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            diagnostics = published(1)
            error = next(item for item in diagnostics if item["code"] == "GTI-S2012")
            assert error["range"]["start"]["line"] == 1, error
            assert error["range"]["start"]["character"] >= lsp_position(source, source.index("std::assert"))["character"], error
            assert not change(2, valid)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(valid, valid.index("assert(true)") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "#macro" in hover["contents"]["value"], hover
            assert "[[deferred]]" not in hover["contents"]["value"], hover
            malformed = "[[deferred]] void old() {}\n" + valid
            assert any(item["code"] == "GTI-S2099" for item in change(3, malformed))
            assert not change(4, valid)
            incomplete = valid.replace("std::assert(true)", "std::assert(")
            assert change(5, incomplete)
            assert not change(6, valid)
            macros = ("#include <std/assert>\n"
                      "namespace application {\n"
                      "  #macro check(int value) { std::assert(value == 7); }\n"
                      "  #macro check(bool value) { std::assert(value); }\n"
                      "}\n"
                      "int main() { /*😀*/ application::check(7); return 0; }\n")
            assert not change(7, macros)
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(macros, macros.index("check(7)") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert "#macro" in hover["contents"]["value"], hover
            assert "int value" in hover["contents"]["value"], hover
            session.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(macros, macros.index("check(7)") + 1)}})
            definition = session.receive_until(lambda message: message.get("id") == 4)["result"]
            locations = definition if isinstance(definition, list) else [definition]
            assert any(location["uri"] == uri and location["range"]["start"]["line"] == 2
                       for location in locations), definition
            session.send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/semanticTokens/full", "params": {
                "textDocument": {"uri": uri}}})
            token_data = session.receive_until(lambda message: message.get("id") == 5)["result"]["data"]
            tokens = semantic_tokens_by_position(token_data)
            for offset in (macros.index("check(int"), macros.index("check(7)")):
                position = lsp_position(macros, offset)
                token = tokens[(position["line"], position["character"])]
                assert token_types[token["type"]] == "macro", (position, token, token_types)
            wrong_type = macros.replace("check(7)", 'check("wrong")')
            diagnostics = change(8, wrong_type)
            error = next(item for item in diagnostics if item["code"] == "GTI-S2012")
            assert error["range"]["start"]["line"] == 5, error
            assert error["range"]["start"]["character"] >= lsp_position(
                wrong_type, wrong_type.index("application::check"))["character"], error
            assert not change(9, macros)
            assert change(10, "#macro unfinished(int value\n" + macros)
            assert not change(11, macros)
        finally:
            session.close()
    print("typed macro LSP diagnostics, UTF-16, hover and overlay recovery passed")


if __name__ == "__main__":
    main()
