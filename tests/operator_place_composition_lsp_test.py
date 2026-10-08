#!/usr/bin/env python3
"""Operator-store semantic diagnostics recover without blocking symbol queries."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession


def main():
    fixture = pathlib.Path(__file__).parent / "fixtures/operator_place_composition.gti"
    source = "// café: exact versioned operator places\n" + fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="gti-operator-place-lsp-") as temporary:
        path = pathlib.Path(temporary) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            result = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in result, result
            return result.get("result")

        def diagnostics(version):
            result = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("version") == version)
            return result["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            assert not [item for item in diagnostics(1) if item.get("severity") == 1]
            lines = source.splitlines()
            line = next(i for i, text in enumerate(lines) if "*cell = *cell + 1;" in text)
            params = {"textDocument": {"uri": uri},
                      "position": {"line": line, "character": lines[line].index("cell")}}
            assert "Cell" in str(request("textDocument/hover", params))
            assert request("textDocument/definition", params)
            change(2, source.replace("mut Cell cell{};", "Cell cell{};"))
            errors = [item for item in diagnostics(2) if item.get("severity") == 1]
            assert errors and all(item.get("code") != "GTI-B0001" for item in errors), errors
            assert any(item.get("code") in ("GTI-S2002", "GTI-S2022") for item in errors), errors
            assert all(item["range"]["start"]["line"] > 0 for item in errors), errors
            assert "Cell" in str(request("textDocument/hover", params))
            change(3, source)
            assert not [item for item in diagnostics(3) if item.get("severity") == 1]
            target_line = next(i for i, text in enumerate(lines) if "cell.slot() = rhs();" in text)
            target_params = {"textDocument": {"uri": uri}, "position": {
                "line": target_line, "character": lines[target_line].index("slot")}}
            assert request("textDocument/hover", target_params)
            assert request("textDocument/definition", target_params)
            change(4, source.replace("cell.slot() = rhs();", "cell.read_slot() = rhs();"))
            errors = [item for item in diagnostics(4) if item.get("severity") == 1]
            assert any(item.get("code") == "GTI-S2002" and
                       item["range"]["start"]["line"] == target_line for item in errors), errors
            assert all(item.get("code") != "GTI-B0001" for item in errors), errors
            change(5, source)
            assert not [item for item in diagnostics(5) if item.get("severity") == 1]
        finally:
            session.close()


if __name__ == "__main__":
    main()
