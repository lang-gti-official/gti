#!/usr/bin/env python3
"""Typed macros use compiler diagnostics, signatures and current overlays."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = ("import std.assert;\n"
              "int main() { /*😀*/ std::assert(1); return 0; }\n")
    valid = source.replace("assert(1)", "assert(true)")
    with tempfile.TemporaryDirectory(prefix="gti-deferred-lsp-") as directory:
        path = (pathlib.Path(directory) / "main.gti").resolve()
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
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            initialized = session.receive_until(lambda message: message.get("id") == 1)["result"]
            assert initialized["capabilities"]["experimental"]["macroExpansionProvider"] == {
                "method": "gti/inspectMacroExpansion", "readOnly": True}, initialized
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
            macros = ("import std.assert;\n"
                      "namespace application {\n"
                      "  /*😀*/ #macro check(int value) { std::assert(value == 7); }\n"
                      "  #macro check(bool value) { std::assert(value); }\n"
                      "}\n"
                      "int main() { /*😀*/ application::check(7); return 0; }\n")
            assert not change(7, macros)
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(macros, macros.index("check(7)") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert "#macro" in hover["contents"]["value"], hover
            assert "int32_t value" in hover["contents"]["value"], hover
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
            keyword = lsp_position(macros, macros.index("#macro"))
            keyword_token = tokens[(keyword["line"], keyword["character"])]
            assert token_types[keyword_token["type"]] == "keyword", keyword_token
            assert keyword_token["length"] == len("#macro"), keyword_token
            assert not any(line == keyword["line"] and keyword["character"] < column
                           < keyword["character"] + len("#macro")
                           for line, column in tokens), tokens
            # The implicit generated void type must not become a written type
            # occurrence over any part of the real six-byte declaration token.
            for index in range(len("#macro")):
                request_id = 6 + index
                session.send({"jsonrpc": "2.0", "id": request_id,
                              "method": "textDocument/hover", "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(macros, macros.index("#macro") + index)}})
                result = session.receive_until(
                    lambda message: message.get("id") == request_id)["result"]
                assert result is None, (index, result)
            wrong_type = macros.replace("check(7)", 'check("wrong")')
            diagnostics = change(8, wrong_type)
            error = next(item for item in diagnostics if item["code"] == "GTI-S2012")
            assert error["range"]["start"]["line"] == 5, error
            assert error["range"]["start"]["character"] >= lsp_position(
                wrong_type, wrong_type.index("application::check"))["character"], error
            assert not change(9, macros)
            assert change(10, "#macro unfinished(int value\n" + macros)
            assert not change(11, macros)

            inspection = ("void use(int value) {}\n"
                          "#macro local(int value) { int saved = value; use(saved); }\n"
                          "#macro relay(int value) { local(value); }\n"
                          "int main() { int saved = 7; /*😀*/ relay(saved); return 0; }\n")
            diagnostics = change(12, inspection)
            assert not diagnostics, diagnostics
            request_id = 50

            def inspect(text, offset, version):
                nonlocal request_id
                request_id += 1
                session.send({"jsonrpc": "2.0", "id": request_id,
                              "method": "gti/inspectMacroExpansion", "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "position": lsp_position(text, offset)}})
                return session.receive_until(lambda message: message.get("id") == request_id)

            offset = inspection.index("relay(saved)")
            expanded = inspect(inspection, offset + 2, 12)["result"]
            assert expanded["readOnly"] and expanded["version"] == 12, expanded
            assert expanded["languageId"] == "gti" and not expanded["truncated"], expanded
            assert expanded["range"] == {
                "start": lsp_position(inspection, offset),
                "end": lsp_position(inspection, offset + len("relay(saved)"))}, expanded
            assert "not executable source" in expanded["text"], expanded
            assert "Nested macro expansion" in expanded["text"], expanded
            assert "saved__macro_local_1 = saved" in expanded["text"], expanded
            assert expanded["declaration"]["uri"] == uri, expanded
            assert expanded["declaration"]["range"]["start"]["line"] == 2, expanded
            assert inspect(inspection, offset, 11)["error"]["code"] == -32801
            assert inspect(inspection, inspection.index("return 0"), 12)["result"] is None

            # Dirty included macro definitions must supply both displayed
            # syntax and provenance; the server must not reread stale disk text.
            policy_path = (pathlib.Path(directory) / "policy.gti").resolve()
            policy_uri = policy_path.as_uri()
            policy_path.write_text((
                                       'export module policy;\n'
                                       '#macro external(int value) { if (value == 1) {} }\n'
                                   ))
            overlay = "#macro external(int value) { if (value == 2) {} }\n"
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": policy_uri, "languageId": "gti", "version": 1, "text": overlay}}})
            included = 'import "policy.gti";\nint main() { external(7); return 0; }\n'
            assert not change(13, included)
            expanded = inspect(included, included.index("external(7)") + 1, 13)["result"]
            assert "(7 == 2)" in expanded["text"], expanded
            assert expanded["declaration"]["uri"] == policy_uri, expanded

            broken = ("#macro broken(int value) { missing(value); }\n"
                      "#macro wrapper(int value) { broken(value); }\n"
                      "int main() { /*😀*/ wrapper(7); return 0; }\n")
            diagnostics = change(14, broken)
            error = next(item for item in diagnostics if item.get("severity", 1) == 1)
            assert error["range"]["start"] == lsp_position(broken, broken.index("wrapper(7)")), error
            related = error.get("relatedInformation", [])
            assert any("macro" in item["message"] and "broken" in item["message"] for item in related), error
            assert any("macro" in item["message"] and "wrapper" in item["message"] for item in related), error
            assert any(item["location"]["range"]["start"]["line"] == 0 for item in related), error
            assert any(item["location"]["range"]["start"]["line"] == 1 for item in related), error

            argument_error = ("#macro ignored(int value) {}\n"
                              "int main() { /*😀*/ ignored(missing_argument); return 0; }\n")
            diagnostics = change(15, argument_error)
            error = next(item for item in diagnostics if item.get("severity", 1) == 1)
            assert error["range"]["start"] == lsp_position(
                argument_error, argument_error.index("missing_argument")), error
            assert not change(16, inspection)
            assert inspect(inspection, inspection.index("relay(saved)"), 15)["error"]["code"] == -32801
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {
                "textDocument": {"uri": uri}}})
            assert inspect(inspection, offset, 16)["result"] is None
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 17,
                                 "text": "int main() { return 0; }\n"}}})
            assert not published(17)
            assert inspect("int main() { return 0; }\n", 1, 17)["result"] is None
        finally:
            session.close()
    print("typed macro LSP diagnostics, UTF-16, hover and overlay recovery passed")


if __name__ == "__main__":
    main()
