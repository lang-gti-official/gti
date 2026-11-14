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
                "struct Borrowed<T, lifetime owner> {\n"
                "  T&<owner> value;\n"
                "  Borrowed(T&<owner> input) : value(input) {}\n"
                "};\n"
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

            def source_range(text, offset, length):
                return {"start": lsp_position(text, offset),
                        "end": lsp_position(text, offset + length)}

            ambiguous = (
                "/* 😀 */ int& choose(int& first, int& second, bool condition) {\n"
                "  if (condition) { return first; } return second;\n}\n")
            errors = change(ambiguous, 9)
            error = next(item for item in errors if item["code"] == "GTI-S2017")
            assert error["range"] == source_range(
                ambiguous, ambiguous.index("choose"), 6), error
            notes = error["relatedInformation"]
            assert len(notes) == 2, notes
            for note, name in zip(notes, ("first", "second")):
                assert name in note["message"], note
                assert note["location"] == {"uri": uri, "range": source_range(
                    ambiguous, ambiguous.index(name), len(name))}, note
            fixed = (
                "/* 😀 */ int&<a> choose<lifetime a>(int&<a> first, "
                "int&<a> second, bool condition) {\n"
                "  if (condition) { return first; } return second;\n}\n")
            assert change(fixed, 10) == []
            assert not request("textDocument/codeAction", {
                "textDocument": {"uri": uri}, "range": error["range"],
                "context": {"diagnostics": [error], "only": ["quickfix"]}})

            mismatch = (
                "/* 😀 */ int&<a> choose<lifetime a, lifetime b>("
                "int&<a> first, int&<b> second) { return second; }\n")
            errors = change(mismatch, 11)
            errors = [item for item in errors if item.get("severity") == 1]
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2127", errors
            error = errors[0]
            assert error["range"] == source_range(
                mismatch, mismatch.rindex("second"), 6), error
            notes = error["relatedInformation"]
            assert len(notes) == 2, notes
            for note, region in zip(notes, ("b", "a")):
                assert note["location"] == {"uri": uri, "range": source_range(
                    mismatch, mismatch.index("<" + region + ">") + 1, 1)}, note
            bounded = mismatch.replace(
                ") { return", ") requires outlives<b, a> { return")
            assert change(bounded, 12) == []
            reborrow = (
                "int main() {\n"
                "  mut int value = 1; mut int& parent = value;\n"
                "  { mut int& ended = parent; ended = 2; }\n"
                "  /* 😀 */ mut int& child = parent;\n"
                "  /* 😀 */ parent = 3;\n"
                "  return child;\n}\n")
            errors = change(reborrow, 13)
            error = next(item for item in errors if item["code"] == "GTI-S2017")
            assert error["range"] == source_range(
                reborrow, reborrow.index("parent = 3"), 6), error
            notes = error["relatedInformation"]
            assert notes[0]["location"] == {"uri": uri, "range": source_range(
                reborrow, reborrow.index("parent;", reborrow.index("child =")), 6)}, notes
            assert notes[1]["location"] == {"uri": uri, "range": source_range(
                reborrow, reborrow.index("return child"), len("return child;"))}, notes
            ended = reborrow.replace("parent = 3;", "int saved = child; parent = 3;")
            ended = ended.replace("return child;", "return saved;")
            assert not [item for item in change(ended, 14)
                        if item.get("severity") == 1]
            loop = (
                "int main() {\n"
                "  mut int value = 1; int& borrowed = value;\n"
                "  for (mut int i = 0; i < 2; i += 1) {\n"
                "    /* 😀 */ int copy = borrowed;\n"
                "    value = copy + 1;\n"
                "  } return 0;\n}\n")
            errors = change(loop, 15)
            error = next(item for item in errors if item["code"] == "GTI-S2017")
            assert any("This borrows 'value'." in item["message"]
                       for item in error["relatedInformation"]), error
            note = next(item for item in error["relatedInformation"]
                        if "later loop iteration" in item["message"])
            assert note["location"] == {"uri": uri, "range": source_range(
                loop, loop.index("int copy"), len("int copy = borrowed;"))}, note
            per_iteration = loop.replace(" int& borrowed = value;", "")
            per_iteration = per_iteration.replace(
                "int copy = borrowed;", "int& borrowed = value; int copy = borrowed;")
            assert not [item for item in change(per_iteration, 16)
                        if item.get("severity") == 1]
            temporary = (
                "import std.string;\nint main() {\n"
                "  /* 😀 */ auto borrowed = std::string(\"hello\").view();\n"
                "  return int(borrowed.size());\n}\n")
            errors = change(temporary, 17)
            error = next(item for item in errors if item["code"] == "GTI-S2045")
            note = next(item for item in error["relatedInformation"]
                        if "temporary owns" in item["message"])
            assert note["location"] == {"uri": uri, "range": source_range(
                temporary, temporary.index(").view"), 1)}, note
            named = temporary.replace(
                "auto borrowed = std::string(\"hello\").view();",
                "std::string owner = std::string(\"hello\"); auto borrowed = owner.view();")
            assert not [item for item in change(named, 18)
                        if item.get("severity") == 1]
            local_return = (
                "int& invalid() {\n"
                "  /* 😀 */ int value = 1;\n"
                "  /* 😀 */ return value;\n}\n")
            errors = change(local_return, 19)
            error = next(item for item in errors if item["code"] == "GTI-S2017")
            assert "local value 'value'" in error["message"], error
            assert error["range"] == source_range(
                local_return, local_return.rindex("value"), 5), error
            assert error["relatedInformation"][0]["location"] == {
                "uri": uri, "range": source_range(
                    local_return, local_return.index("value"), 5)}, error
            assert change(local_return.replace("int& invalid", "int invalid"), 20) == []
            closure_escape = (
                "import std.functional;\n"
                "std::function_ref<int()> invalid() {\n"
                "  int value = 1;\n"
                "  /* 😀 */ auto callback = [&value]() -> int { return value; };\n"
                "  std::function_ref<int()> result{callback};\n"
                "  /* 😀 */ return result;\n}\n"
                "int main() { auto call = invalid(); return call(); }\n")
            errors = change(closure_escape, 21)
            error = next(item for item in errors if item["code"] == "GTI-S2127")
            assert error["range"] == source_range(
                closure_escape, closure_escape.index("result;"), 6), error
            note = next(item for item in error["relatedInformation"]
                        if "Local 'callback'" in item["message"])
            assert note["location"] == {"uri": uri, "range": source_range(
                closure_escape, closure_escape.index("callback"), 8)}, note
            assert "cannot extend local storage" in error["message"], error
            assert change(SOURCE, 22) == []
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
