#!/usr/bin/env python3
"""One method identity across declarations, bodies, calls and live overlays."""

import json
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, open_document, write
from lsp_smoke_test import has_only_include_advice, LspSession, initialize_session, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-split-lsp-") as temporary:
        root = pathlib.Path(temporary)
        api, body, entry = (root / name for name in ("api.gti", "body.gti", "main.gti"))
        interface = ("export module api;\nexport struct Parcel<T> { T value; };\n"
                     "export interface Factory { static int make(int number = 7); "
                     "static Parcel<int> parcel(); };\n")
        implementation = ("import api;\n/* UTF-16: 🦉 */ int Factory::make(int value) { return value; }\n"
                          "Parcel < int > Factory::parcel() { Parcel<int> result = {.value = 7}; return result; }\n")
        source = "import api;\nint main() { return Factory::make() - 7; }\n"
        write(api, interface)
        write(body, implementation)
        write(entry, source)
        write(root / "gti.toml", 'manifest-version = 1\n[package]\nname = "split"\n'
              'version = "0.1.0"\n[modules]\napi = "api.gti"\n'
              '[module-implementations]\napi = ["body.gti"]\n'
              '[targets.split]\nkind = "executable"\nroot = "main.gti"\n')
        session = LspSession(sys.argv[1])
        request_id = 20

        def request(method, uri, text, offset, **extra):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, offset), **extra}})
            response = session.receive_until(lambda value: value.get("id") == request_id)
            assert "error" not in response, response
            return response["result"]

        def change(uri, version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})

        try:
            initialize_session(session, capabilities={
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}})
            uri = open_document(session, entry, source)
            assert diagnostics_for(session, uri, 1) == []
            where = request("textDocument/definition", uri, source, source.index("make") + 1)
            assert where["uri"] == body.resolve().as_uri(), where
            assert where["range"]["start"] == lsp_position(implementation, implementation.index("make")), where
            hover = request("textDocument/hover", uri, source, source.index("make") + 1)
            assert "make" in json.dumps(hover), hover
            references = request("textDocument/references", uri, source, source.index("make") + 1,
                                 context={"includeDeclaration": True})
            assert {item["uri"] for item in references} == {
                path.resolve().as_uri() for path in (api, body, entry)}, references

            body_uri = open_document(session, body, implementation)
            body_diagnostics = diagnostics_for(session, body_uri, 1)
            assert body_diagnostics == [], body_diagnostics
            # Public method rename retains the existing conservative boundary;
            # local parameters belong to the definition, not its prototype.
            renamed = request("textDocument/rename", body_uri, implementation,
                              implementation.index("value") + 1, newName="input")
            edits = renamed["changes"]
            assert set(edits) == {body_uri} and len(edits[body_uri]) == 2, edits
            broken = implementation.replace("return value", "return absent")
            change(body_uri, 2, broken)
            errors = diagnostics_for(session, body_uri, 2)
            assert any("absent" in item["message"] for item in errors), errors
            change(body_uri, 3, implementation)
            assert diagnostics_for(session, body_uri, 3) == []

            mismatch = implementation.replace("int Factory::make", "bool Factory::make").replace("return value", "return true")
            change(body_uri, 4, mismatch)
            errors = diagnostics_for(session, body_uri, 4)
            diagnostic = next((item for item in errors if item.get("code") == "GTI-S2120"), None)
            assert diagnostic is not None, errors
            assert diagnostic["range"]["start"] == lsp_position(mismatch, mismatch.index("make")), diagnostic
            assert diagnostic.get("relatedInformation"), diagnostic
            change(body_uri, 5, implementation)
            assert diagnostics_for(session, body_uri, 5) == []
            where = request("textDocument/definition", body_uri, implementation, implementation.index("Factory") + 1)
            assert where["uri"] == api.resolve().as_uri(), where

            formatted = request("textDocument/formatting", body_uri, implementation, 0,
                                options={"tabSize": 2, "insertSpaces": True})
            formatted_text = formatted[0]["newText"]
            assert "Parcel<int> Factory::parcel() {" in formatted_text, formatted_text
            change(body_uri, 6, formatted_text)
            assert diagnostics_for(session, body_uri, 6) == []
            repeated = request("textDocument/formatting", body_uri, formatted_text, 0,
                               options={"tabSize": 2, "insertSpaces": True})
            assert repeated == [] or repeated[0]["newText"] == formatted_text, repeated
        finally:
            session.close()


def test_signature_type_visibility(executable):
    with tempfile.TemporaryDirectory(prefix="gti-split-type-lsp-") as temporary:
        root = pathlib.Path(temporary)
        api, body = root / "api.gti", root / "body.gti"
        write(api, "#include <std/memory>\nnamespace demo { interface Factory {\n"
              "  static std::unique_ptr<Factory> make();\n}; }\n")
        source = ('#include "api.gti"\nnamespace demo {\n'
                  '/* UTF-16: 🦉 */ std::unique_ptr<Factory> Factory::make() {\n'
                  '  return std::unique_ptr<Factory>(nullptr);\n}\n}\n'
                  'int main() { auto owner = demo::Factory::make(); return 0; }\n')
        write(body, source)
        session = LspSession(executable)
        try:
            initialize_session(session, capabilities={
                "workspace": {"workspaceEdit": {"documentChanges": True}},
                "textDocument": {
                    "publishDiagnostics": {"relatedInformation": True, "dataSupport": True},
                    "codeAction": {"codeActionLiteralSupport": {
                        "codeActionKind": {"valueSet": ["quickfix"]}}}}})
            uri = open_document(session, body, source)
            errors = diagnostics_for(session, uri, 1)
            diagnostic = next((item for item in errors if item.get("code") == "GTI-S2024"), None)
            assert diagnostic is not None, errors
            assert not any(item.get("code") == "GTI-S2120" for item in errors), errors
            offset = source.index("unique_ptr")
            assert diagnostic["range"] == {
                "start": lsp_position(source, offset),
                "end": lsp_position(source, offset + len("unique_ptr"))}, diagnostic
            assert diagnostic["relatedInformation"], diagnostic
            assert any("#include <std/memory>" in hint
                       for hint in diagnostic["data"]["hints"]), diagnostic

            def actions(request_id):
                session.send({"jsonrpc": "2.0", "id": request_id,
                              "method": "textDocument/codeAction", "params": {
                                  "textDocument": {"uri": uri}, "range": diagnostic["range"],
                                  "context": {"diagnostics": [diagnostic], "only": ["quickfix"]}}})
                return session.receive_until(lambda value: value.get("id") == request_id)["result"]

            fixes = actions(20)
            assert len(fixes) == 1, fixes
            assert fixes[0]["title"] == "Add #include <std/memory>", fixes
            document = fixes[0]["edit"]["documentChanges"][0]
            assert document["textDocument"] == {"uri": uri, "version": 1}, document
            assert document["edits"] == [{
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 0, "character": 0}},
                "newText": "#include <std/memory>\n"}], document
            corrected = "#include <std/memory>\n" + source
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": corrected}]}})
            assert has_only_include_advice(diagnostics_for(session, uri, 2))
            assert actions(21) == [], "stale signature diagnostics must not offer edits"
        finally:
            session.close()


if __name__ == "__main__":
    main()
    test_signature_type_visibility(sys.argv[1])
