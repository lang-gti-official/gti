#!/usr/bin/env python3
"""One method identity across declarations, bodies, calls and live overlays."""

import json
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, open_document, write
from lsp_smoke_test import LspSession, initialize_session, lsp_position


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


if __name__ == "__main__":
    main()
