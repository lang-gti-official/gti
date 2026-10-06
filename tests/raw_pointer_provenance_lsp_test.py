#!/usr/bin/env python3
"""Raw-origin diagnostics are shared, source-located and generation scoped."""

import os
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    valid = """int main() {
  mut const int* pointer = nullptr;
  unsafe {
    int value = 42; pointer = &value;
    /*😀*/ return *pointer;
  }
}
"""
    invalid = valid.replace("int value = 42; pointer = &value;",
                            "{ int value = 42; pointer = &value; }")
    with tempfile.TemporaryDirectory(prefix="gti-raw-provenance-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(valid)
        uri = path.as_uri()
        session = LspSession(sys.argv[1], ["--raw-pointer-audit=strict"])

        def published(version):
            result = session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)
            return [item for item in result["params"]["diagnostics"]
                    if item.get("severity") == 1]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": source}]}})

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {
                              "workspace": {"workspaceEdit": {"documentChanges": True}},
                              "textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            reply = session.receive_until(lambda message: message.get("id") == 1)
            assert "error" not in reply, reply
            token_types = reply["result"]["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": valid}}})
            assert not published(1)
            change(2, invalid)
            errors = published(2)
            error = next((item for item in errors if item.get("code") == "GTI-S2101"), None)
            assert error and "scope has ended" in error["message"], errors
            assert error["range"]["start"] == lsp_position(invalid, invalid.index("*pointer")), error
            related = error.get("relatedInformation", [])
            assert any(item["location"]["uri"] == uri and
                       item["location"]["range"]["start"] ==
                       lsp_position(invalid, invalid.index("&value"))
                       for item in related), error
            change(3, valid)
            assert not published(3)
            change(4, invalid.replace("*pointer", "*"))
            assert published(4)
            change(5, valid)
            assert not published(5)
            safe_address = """int main() {
  int value = 42;
  const int* pointer = &value;
  return pointer == nullptr ? 1 : 0;
}
"""
            change(6, safe_address)
            assert not published(6)
            raw_under_address = safe_address.replace("&value", "&*(&value)")
            change(7, raw_under_address)
            errors = published(7)
            assert any(item.get("code") == "GTI-S2055" for item in errors), errors
            change(8, safe_address)
            assert not published(8)
            contract_source = """/*😀*/ [[borrowed_from(right)]] int& choose(int& left, int& right) { return right; }
struct Holder {
  int value = 7;
  [[borrowed_from(this)]] int& get() { return this.value; }
};
int borrowed_from(int value) { return value; }
int main() {
  int left = 3;
  int right = 4;
  Holder holder{};
  return choose(left, right) + holder.get() + borrowed_from(1);
}
"""
            change(9, contract_source)
            assert not published(9)
            sequence = 100

            def request(method, params):
                nonlocal sequence
                sequence += 1
                session.send({"jsonrpc": "2.0", "id": sequence,
                              "method": method, "params": params})
                reply = session.receive_until(lambda item: item.get("id") == sequence)
                assert "error" not in reply, reply
                return reply["result"]

            def hover(source, offset):
                return str(request("textDocument/hover", {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, offset + 1)}))

            for offset in [contract_source.index("choose("),
                           contract_source.rindex("choose(")]:
                assert "[[borrowed_from(right)]]" in hover(contract_source, offset)
            for offset in [contract_source.index("get()"),
                           contract_source.rindex("get()")]:
                assert "[[borrowed_from(this)]]" in hover(contract_source, offset)

            def token_type_at(source, offset):
                result = request("textDocument/semanticTokens/full", {
                    "textDocument": {"uri": uri}})
                tokens = semantic_tokens_by_position(result["data"])
                position = lsp_position(source, offset)
                token = tokens[(position["line"], position["character"])]
                return token_types[token["type"]]

            assert token_type_at(contract_source, contract_source.index("borrowed_from")) == "decorator"
            assert token_type_at(contract_source, contract_source.index("borrowed_from(this)")) == "decorator"
            assert token_type_at(contract_source, contract_source.rindex("borrowed_from(1)")) == "function"
            incomplete = contract_source.replace("borrowed_from(right)", "borrowed_from(right", 1)
            change(10, incomplete)
            assert any(item.get("code") == "GTI-P0001" for item in published(10))
            assert token_type_at(incomplete, incomplete.index("borrowed_from")) == "decorator"
            change(11, contract_source)
            assert not published(11)
            assert "[[borrowed_from(right)]]" in hover(contract_source, contract_source.rindex("choose("))
            origin = contract_source.index("right)")
            declaration = contract_source.index("right) { return")
            returned = contract_source.index("return right") + len("return ")
            owner_params = {"textDocument": {"uri": uri},
                            "position": lsp_position(contract_source, origin + 1)}
            definition = request("textDocument/definition", owner_params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"] == lsp_position(contract_source, declaration), definition
            references = request("textDocument/references", {
                **owner_params, "context": {"includeDeclaration": True}})
            expected_positions = [lsp_position(contract_source, offset)
                                  for offset in [origin, declaration, returned]]
            assert [item["range"]["start"] for item in references] == expected_positions, references
            assert all(item["uri"] == uri for item in references), references
            edits = request("textDocument/rename", {
                **owner_params, "newName": "selected_owner"})
            assert len(edits["documentChanges"]) == 1, edits
            document_edit = edits["documentChanges"][0]
            assert document_edit["textDocument"]["uri"] == uri, edits
            changes = document_edit["edits"]
            assert sorted((item["range"]["start"]["line"], item["range"]["start"]["character"])
                          for item in changes) == sorted((item["line"], item["character"])
                                                       for item in expected_positions), edits
            assert all(item["newText"] == "selected_owner" for item in changes), edits
            # Rename only this function's parameter, not the equal-spelled local
            # in main. Apply the edits as a new snapshot and verify its contract.
            renamed = contract_source
            for offset in sorted([origin, declaration, returned], reverse=True):
                renamed = renamed[:offset] + "selected_owner" + renamed[offset + len("right"):]
            change(12, renamed)
            assert not published(12)
            assert "[[borrowed_from(selected_owner)]]" in hover(renamed, renamed.rindex("choose("))
            helper_source = """unsafe int read_address(const int* pointer) {
  unsafe { return *pointer; }
}
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  unsafe { /*😀*/ return read_address(pointer); }
}
"""
            change(13, helper_source)
            errors = published(13)
            error = next((item for item in errors if item.get("code") == "GTI-S2102"), None)
            assert error, errors
            invocation = helper_source.rindex("read_address(pointer)")
            first = lsp_position(helper_source, invocation)
            last = lsp_position(helper_source, invocation + len("read_address(pointer)"))
            start = error["range"]["start"]
            end = error["range"]["end"]
            assert start["line"] == end["line"] == first["line"] == last["line"], error
            assert first["character"] <= start["character"] < end["character"] <= last["character"], error
            assert any(item["location"]["uri"] == uri and
                       item["location"]["range"]["start"] ==
                       lsp_position(helper_source, helper_source.index("&dead"))
                       for item in error.get("relatedInformation", [])), error
            helper_live = helper_source.replace("{ int dead = 7; pointer = &dead; }",
                                                "int dead = 7; pointer = &dead;")
            change(14, helper_live)
            assert not published(14)
            inert_source = helper_source.replace(
                "unsafe int read_address(const int* pointer) {\n  unsafe { return *pointer; }\n}",
                "bool is_null(const int* pointer) { return pointer == nullptr; }")
            inert_source = inert_source.replace("read_address(pointer)", "is_null(pointer) ? 1 : 0")
            change(15, inert_source)
            assert not published(15)
        finally:
            session.close()


if __name__ == "__main__":
    main()
