#!/usr/bin/env python3
"""Lifetime binders retain exact navigation identities through editor changes."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


SOURCE = (
    "/* UTF-16: \U0001f600 */ int&<sourceRegion> identity<lifetime sourceRegion>(int&<sourceRegion> value) {\n"
    "  int&<sourceRegion> local = value;\n"
    "  return local;\n"
    "}\n"
    "int&<sourceRegion> other<lifetime sourceRegion>(int&<sourceRegion> value) { return value; }\n"
    "int lifetime = 1;\n"
)


def main():
    with tempfile.TemporaryDirectory(prefix="gti-lifetime-lsp-") as directory:
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

        def at(text, offset):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, offset)}

        def diagnostics(version):
            reply = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)
            return reply["params"]["diagnostics"]

        def change(text, version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})
            return diagnostics(version)

        try:
            initialized = request("initialize", {"capabilities": {
                "workspace": {"workspaceEdit": {"documentChanges": True}},
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}},
            }})
            legend = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": SOURCE}}})
            assert diagnostics(1) == []
            binder_offset = SOURCE.index("lifetime sourceRegion") + len("lifetime ")
            use_offset = SOURCE.index("sourceRegion")
            use = at(SOURCE, use_offset)
            hover = request("textDocument/hover", use)
            assert "lifetime sourceRegion" in str(hover), hover
            declaration_hover = request("textDocument/hover", at(SOURCE, SOURCE.index("identity")))
            assert "identity<lifetime sourceRegion>" in str(declaration_hover), declaration_hover
            assert "int32_t&<sourceRegion>" in str(declaration_hover), declaration_hover
            definition = request("textDocument/definition", use)
            location = definition[0] if isinstance(definition, list) else definition
            assert location == {"uri": uri, "range": {
                "start": lsp_position(SOURCE, binder_offset),
                "end": lsp_position(SOURCE, binder_offset + len("sourceRegion"))}}, definition
            references = request("textDocument/references", {
                **use, "context": {"includeDeclaration": True}})
            assert len(references) == 4, references
            assert all(item["range"]["start"]["line"] <= 1 for item in references), references
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            for item in references:
                position = item["range"]["start"]
                token = tokens[(position["line"], position["character"])]
                assert legend[token["type"]] == "typeParameter", token
            variable_position = lsp_position(SOURCE, SOURCE.rindex("lifetime"))
            variable_token = tokens[(variable_position["line"], variable_position["character"])]
            assert legend[variable_token["type"]] == "variable", variable_token
            renamed = request("textDocument/rename", {**use, "newName": "inputRegion"})
            document = renamed["documentChanges"][0]
            assert document["textDocument"] == {"uri": uri, "version": 1}, document
            assert len(document["edits"]) == 4, document
            assert all(edit["newText"] == "inputRegion" for edit in document["edits"]), document
            for name in ("_", "static"):
                rejected = request("textDocument/rename", {**use, "newName": name}, expect_error=True)
                assert rejected["code"] == -32803, rejected

            incomplete = SOURCE.replace("int&<sourceRegion> local", "int&<sou> local")
            assert change(incomplete, 2)
            position = incomplete.index("<sou> local") + len("<sou")
            completion = request("textDocument/completion", at(incomplete, position))
            items = completion["items"] if isinstance(completion, dict) else completion
            candidate = next(item for item in items if item["label"] == "sourceRegion")
            assert candidate["kind"] == 25 and "lifetime sourceRegion" in candidate["detail"], candidate
            assert all(item["label"] not in {"value", "local"} for item in items), items
            broken = SOURCE.replace("int&<sourceRegion> local", "int&<> local")
            assert change(broken, 3)
            assert change(SOURCE, 4) == []
            assert request("textDocument/definition", use) == definition
            unknown = SOURCE.replace("int&<sourceRegion> local", "int&<missingRegion> local")
            errors = change(unknown, 5)
            offset = unknown.index("missingRegion")
            assert any(error["range"] == {
                "start": lsp_position(unknown, offset),
                "end": lsp_position(unknown, offset + len("missingRegion"))}
                and "lifetime" in error["message"].lower() for error in errors), errors
            assert change(SOURCE, 6) == []
            contracts = SOURCE + (
                "struct Borrowed<T, lifetime owner> { T&<owner> value; };\n"
                "using Forever = Borrowed<int, static>;\n"
                "using Read = for<lifetime fresh> int&<fresh>(int&<fresh>);\n"
                "concept Related<T, lifetime region> = outlives<T, region>;\n"
                "struct Receiver { int value; int&<this> read() { return value; } };\n"
            )
            assert change(contracts, 7) == []
            forever = request("textDocument/hover", at(contracts, contracts.index("Forever")))
            assert "Borrowed<int32_t, static>" in str(forever), forever
            callback = request("textDocument/hover", at(contracts, contracts.index("using Read") + 6))
            assert "for<lifetime fresh> int32_t&<fresh>(int32_t&<fresh>)" in str(callback), callback
            concept = request("textDocument/hover", at(contracts, contracts.index("Related")))
            assert "Related<T, lifetime region>" in str(concept), concept
            receiver = request("textDocument/hover", at(contracts, contracts.index("read()")))
            assert "int32_t&<this>" in str(receiver), receiver
            quantified = at(contracts, contracts.index("int&<fresh>") + len("int&<"))
            quantified_references = request("textDocument/references", {
                **quantified, "context": {"includeDeclaration": True}})
            assert len(quantified_references) == 3, quantified_references
            assert change(SOURCE, 8) == []
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": SOURCE}}})
            assert diagnostics(1) == []
            assert request("textDocument/definition", use) == definition
        finally:
            session.close()


if __name__ == "__main__":
    main()
