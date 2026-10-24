#!/usr/bin/env python3
"""Compiler-owned general requirement signatures survive editor recovery."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """#include <std/meta>
// UTF-16 positioning: 🧮
concept sums<T> = requires(T& left, T& right) { { left + right } -> T; };
concept integer_value<T> = std::meta::is_integral<T>();
concept complete_object<T> = std::meta::has_object_layout<T>();
concept detached_object<T> = complete_object<T> && std::meta::is_borrow_free<T>();
concept cleanup_needed<T> = std::meta::has_active_cleanup<T>();
concept dynamic_object<T> = std::meta::is_polymorphic<T>();
concept integer_domain<T> = std::meta::is_signed_integral<T>() || std::meta::is_unsigned_integral<T>();
T increment<T>(T value) requires integer_domain<T> { return value + 1; }
T add<sums T>(T& left, T& right) { return left + right; }
T twice<T>(T value) requires std::meta::is_integral<T>() { return value + value; }
concept raw_read<T> = requires(T& value) { { unsafe { value.raw() } } -> int; };
concept editable<C, T> = requires(mut C& owner) { { owner.edit() } -> mut T& [[borrowed_from(owner)]]; };
int main() { int left = 3; int right = 4; return add(left, right) == twice(left) + 1 ? 0 : 1; }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-general-requires-lsp-") as directory:
        uri = (pathlib.Path(directory) / "requirements.gti").as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def change(source, version):
            if version == 1:
                params = {"textDocument": {"uri": uri, "languageId": "gti",
                                            "version": version, "text": source}}
                method = "textDocument/didOpen"
            else:
                params = {"textDocument": {"uri": uri, "version": version},
                          "contentChanges": [{"text": source}]}
                method = "textDocument/didChange"
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)["params"]["diagnostics"]

        def hover(source, position):
            return request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": lsp_position(source, position)})

        try:
            request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert change(SOURCE, 1) == []
            contract = hover(SOURCE, SOURCE.index("sums T>"))
            assert "left + right" in str(contract) and "-> T" in str(contract), contract
            predicate = hover(SOURCE, SOURCE.index("integer_value<T>"))
            assert "std::meta::is_integral<T>()" in str(predicate), predicate
            callee_position = SOURCE.index("is_integral<T>")
            callee = hover(SOURCE, callee_position)
            assert "is_integral" in str(callee) and "bool" in str(callee), callee
            definition = request("textDocument/definition", {
                "textDocument": {"uri": uri},
                "position": lsp_position(SOURCE, callee_position)})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"].endswith("/std/meta.gti"), definition
            for name in ("has_object_layout", "is_borrow_free", "has_active_cleanup", "is_polymorphic"):
                position = SOURCE.index(name + "<T>")
                query = hover(SOURCE, position)
                assert name in str(query) and "bool" in str(query), query
                definition = request("textDocument/definition", {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(SOURCE, position)})
                location = definition[0] if isinstance(definition, list) else definition
                assert location["uri"].endswith("/std/meta.gti"), definition
            selected = hover(SOURCE, SOURCE.rindex("twice(left)"))
            assert "is_integral<int32_t>()" in str(selected), selected
            raw_contract = hover(SOURCE, SOURCE.index("raw_read<T>"))
            assert "unsafe { value.raw() }" in str(raw_contract), raw_contract
            borrowed_contract = hover(SOURCE, SOURCE.index("editable<C, T>"))
            assert "-> mut T&" in str(borrowed_contract) and "borrowed_from(owner)" in str(borrowed_contract), borrowed_contract
            origin_position = SOURCE.index("owner)]]")
            origin_definition = request("textDocument/definition", {
                "textDocument": {"uri": uri},
                "position": lsp_position(SOURCE, origin_position)})
            origin_location = origin_definition[0] if isinstance(origin_definition, list) else origin_definition
            assert origin_location["range"]["start"] == lsp_position(SOURCE, SOURCE.index("owner) {")), origin_definition
            origin_rename = request("textDocument/rename", {
                "textDocument": {"uri": uri},
                "position": lsp_position(SOURCE, origin_position), "newName": "source"})
            assert len(origin_rename["changes"][uri]) == 3, origin_rename
            use_position = lsp_position(SOURCE, SOURCE.index("left + right"))
            definition = request("textDocument/definition", {
                "textDocument": {"uri": uri}, "position": use_position})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == lsp_position(
                SOURCE, SOURCE.index("left, T&")), definition
            rename = request("textDocument/rename", {
                "textDocument": {"uri": uri}, "position": use_position,
                "newName": "lhs"})
            edits = rename["changes"][uri]
            assert len(edits) == 2 and all(
                edit["newText"] == "lhs" and edit["range"]["start"]["line"] == 2
                for edit in edits), rename

            invalid = SOURCE.replace("left + right } -> T", "left + right } -> bool")
            diagnostics = change(invalid, 2)
            assert any(item["code"] == "GTI-S2029" for item in diagnostics), diagnostics
            assert not any(item["code"] == "GTI-B0001" for item in diagnostics), diagnostics
            assert any(item.get("relatedInformation") for item in diagnostics), diagnostics
            assert change(SOURCE, 3) == []

            incomplete = SOURCE.replace("left + right } -> T", "left + } -> T")
            assert change(incomplete, 4)
            hover(incomplete, incomplete.index("integer_value<T>"))
            assert change(SOURCE, 5) == []
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            if edits:
                formatted = edits[0]["newText"]
                assert "std::meta::is_integral<T>()" in formatted, formatted
                assert change(formatted, 6) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()
