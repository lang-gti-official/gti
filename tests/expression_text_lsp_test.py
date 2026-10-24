#!/usr/bin/env python3
"""Value macros and expression metadata share current compiler LSP records."""

import os
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    repository = pathlib.Path(__file__).resolve().parents[1]
    library = repository / "stdlib"
    source = (
        "#include <std/source_location>\n"
        "#macro int choose(bool condition, int value) {\n"
        "  if (condition) { return value; } return 0;\n"
        "}\n"
        "#macro std::string_view quoted<T>(T expression) {\n"
        "  return std::expression_text(expression);\n"
        "}\n"
        "int main() {\n"
        "  mut int counter = 0; int answer = choose(true, 7);\n"
        "  /*😀*/ std::string_view text = std::expression_text(++counter);\n"
        "  std::string_view forwarded = quoted(answer /* λ */ + 1);\n"
        "  return answer + counter;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-expression-text-lsp-") as directory:
        path = (pathlib.Path(directory) / "main.gti").resolve()
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(
            sys.argv[1], environment=dict(os.environ, GTI_STDLIB_PATH=str(library)))
        request_id = 1

        def published(version):
            diagnostics = session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version,
                timeout=30)["params"]["diagnostics"]
            # An unevaluated mutable expression may legitimately leave an
            # unused-mut warning; this gate follows semantic error clearing.
            return [item for item in diagnostics if item.get("severity", 1) == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}]}})
            return published(version)

        def request(method, text, offset):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, offset)}})
            return session.receive_until(
                lambda message: message.get("id") == request_id, timeout=30)["result"]

        def locations(result):
            return result if isinstance(result, list) else [result]

        try:
            session.send({"jsonrpc": "2.0", "id": request_id, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == request_id)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": source}}})
            diagnostics = published(1)
            assert not diagnostics, diagnostics

            choose_call = source.index("choose(true")
            hover = request("textDocument/hover", source, choose_call + 1)
            signature = hover["contents"]["value"]
            assert "#macro int32_t choose(bool condition, int32_t value)" in signature, hover
            definitions = locations(request("textDocument/definition", source, choose_call + 1))
            assert any(item["uri"] == uri and item["range"]["start"] ==
                       lsp_position(source, source.index("choose(bool"))
                       for item in definitions), definitions

            text_call = source.index("expression_text(++counter)")
            hover = request("textDocument/hover", source, text_call + 1)
            signature = hover["contents"]["value"]
            assert "#macro" in signature and "string_view" in signature and "expression_text" in signature, hover
            definitions = locations(request("textDocument/definition", source, text_call + 1))
            declaration_path = (library / "std" / "source_location.gti").resolve()
            declaration = declaration_path.read_text()
            declaration_offset = declaration.index(" expression_text<T>") + 1
            assert any(item["uri"] == declaration_path.as_uri() and
                       item["range"]["start"] == lsp_position(declaration, declaration_offset)
                       for item in definitions), definitions

            missing = source.replace("expression_text(++counter)",
                                     "expression_text(missing_expression)")
            diagnostics = change(2, missing)
            expected = lsp_position(missing, missing.index("missing_expression"))
            assert any(item["range"]["start"] == expected for item in diagnostics), diagnostics
            assert not change(3, source)

            invalid = source.replace("expression_text(++counter)",
                                     "expression_text(counter + true)")
            diagnostics = change(4, invalid)
            assert any(item.get("severity", 1) == 1 for item in diagnostics), diagnostics
            assert not change(5, source)

            synthesized = source.replace("expression_text(expression)",
                                         "expression_text(expression + 1)")
            diagnostics = change(6, synthesized)
            error = next(item for item in diagnostics if item["code"] == "GTI-S2019"
                         and "complete written source expression" in item["message"])
            invocation = synthesized.index("quoted(answer")
            assert error["range"] == {
                "start": lsp_position(synthesized, invocation),
                "end": lsp_position(synthesized, invocation + len("quoted")),
            }, error
            assert error.get("relatedInformation"), error
            assert not change(7, source)

            incomplete = source.replace("std::expression_text(++counter)",
                                        "std::expression_text(")
            assert change(8, incomplete)
            assert not change(9, source)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {
                "textDocument": {"uri": uri}}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 10,
                                 "text": source}}})
            assert not published(10)
            hover = request("textDocument/hover", source, text_call + 1)
            assert "expression_text" in hover["contents"]["value"], hover

            explicit_fallback = source.replace(
                "expression_text(expression)",
                'expression_text(expression + 1, "composed condition")')
            assert not change(11, explicit_fallback)
            dynamic_fallback = explicit_fallback.replace(
                '"composed condition"', "runtime_fallback()")
            dynamic_fallback = dynamic_fallback.replace(
                "#macro int choose",
                'std::string_view runtime_fallback() { return "dynamic"; }\n'
                "#macro int choose")
            diagnostics = change(12, dynamic_fallback)
            assert any(item["code"] == "GTI-S2019" and
                       "compile-time literal-backed" in item["message"]
                       for item in diagnostics), diagnostics
            assert not change(13, explicit_fallback)
        finally:
            session.close()
    print("expression text LSP signatures, definitions, UTF-16 and recovery passed")


if __name__ == "__main__":
    main()
