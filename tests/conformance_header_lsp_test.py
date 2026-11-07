#!/usr/bin/env python3
"""One conformance-header diagnostic, import help, UTF-16 spans and clearing."""

from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-conformance-header-") as directory:
        root = Path(directory).resolve()
        concept = root / "measure.gti"
        concept.write_text((
                               'export module measure;\n'
                               'export concept measurable<T> { int measure(T& item); bool is_empty(T& item); };\n'
                           ))
        (root / "middle.gti").write_text((
                                             'export module middle;\n'
                                             'import "measure.gti";\n'
                                         ))
        uri = (root / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        version = 0

        def publish(source):
            nonlocal version
            version += 1
            document = {"uri": uri, "version": version}
            if version == 1:
                document.update(languageId="gti", text=source)
                method = "textDocument/didOpen"
                params = {"textDocument": document}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": document, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)["params"]["diagnostics"]

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            for members in ("", "int measure(Rope& item) { return 1; }\n"
                                "bool is_empty(Rope& item) { return false; }\n"):
                source = ('import "middle.gti";\nstruct Rope {};\n'
                          '/* 😀 */ for concept measurable<Rope> {\n' + members + '};\n')
                errors = [item for item in publish(source) if item.get("severity") == 1]
                assert len(errors) == 1 and errors[0]["code"] == "GTI-S2024", errors
                error = errors[0]
                start = source.index("measurable")
                assert error["range"] == {
                    "start": lsp_position(source, start),
                    "end": lsp_position(source, start + len("measurable"))}, error
                declaration = concept.read_text()
                declared = declaration.index("measurable")
                assert any(item["location"] == {
                    "uri": concept.as_uri(),
                    "range": {"start": lsp_position(declaration, declared),
                              "end": lsp_position(declaration, declared + len("measurable"))}}
                    for item in error.get("relatedInformation", [])), error
                assert "import its public module interface" in error["message"], error
                assert "help: Add an import of the defining interface" in error["message"], error
                unknown = source.replace("measurable<Rope>", "missing<Rope>")
                errors = [item for item in publish(unknown) if item.get("severity") == 1]
                assert len(errors) == 1 and errors[0]["code"] == "GTI-S2123", errors
            valid = source.replace('"middle.gti"', '"measure.gti"')
            cleared = publish(valid)
            assert cleared == [], ("correcting the import clears the error", cleared)
            lifetime_source = (
                "concept chooser<T> { int&<picked> choose<lifetime picked>(int&<picked> value); };\n"
                "struct Reading<lifetime storage> { int&<storage> value; Reading(int&<storage> value) : value(value) {} };\n"
                "for concept chooser<Reading> { int&<selected> choose<lifetime selected>(int&<selected> value) { return value; } };\n"
                "int&<user> relay<lifetime head, lifetime user>(Reading<head>& reading, int&<user> value) {\n"
                "  return chooser<Reading<head>>::choose<user>(value);\n"
                "}\n"
            )
            errors = publish(lifetime_source)
            assert errors == [], errors
            offset = lifetime_source.rindex("choose<user>")
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(lifetime_source, offset)}})
            hover = session.receive_until(lambda item: item.get("id") == 2)
            assert "error" not in hover, hover
            rendered = str(hover.get("result"))
            assert "choose<user>" in rendered and "$lifetime" not in rendered, hover
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/documentSymbol",
                          "params": {"textDocument": {"uri": uri}}})
            symbols = session.receive_until(lambda item: item.get("id") == 3)
            assert "error" not in symbols and "$lifetime" not in str(symbols), symbols
            assert "for concept chooser<Reading>" in str(symbols), symbols
            pattern_source = (
                "class Callback<R(Args...), lifetime environment> {\n"
                "  int&<environment> value;\n"
                "  Callback(int&<environment> input) : value(input) {}\n"
                "};\n"
            )
            assert publish(pattern_source) == [], "patterned lifetime suffix has no editor-only diagnostic"
            session.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/hover",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(pattern_source, pattern_source.index("Callback"))}})
            hover = session.receive_until(lambda item: item.get("id") == 4)
            assert "error" not in hover, hover
            assert "lifetime environment" in str(hover.get("result")), hover
            assert "$lifetime" not in str(hover.get("result")), hover
            session.send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/documentSymbol",
                          "params": {"textDocument": {"uri": uri}}})
            symbols = session.receive_until(lambda item: item.get("id") == 5)
            assert "error" not in symbols and "lifetime environment" in str(symbols), symbols
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
