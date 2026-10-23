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
T add<T>(T& left, T& right) requires sums<T> { return left + right; }
T twice<T>(T value) requires std::meta::is_integral<T>() { return value + value; }
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
            contract = hover(SOURCE, SOURCE.index("sums<T> {"))
            assert "left + right" in str(contract) and "-> T" in str(contract), contract
            predicate = hover(SOURCE, SOURCE.index("integer_value<T>"))
            assert "std::meta::is_integral<T>()" in str(predicate), predicate
            selected = hover(SOURCE, SOURCE.rindex("twice(left)"))
            assert "is_integral<int32_t>()" in str(selected), selected

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
