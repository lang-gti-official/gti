#!/usr/bin/env python3
"""Temporary argument diagnostics, navigation and recovery share semantic facts."""
from pathlib import Path
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position

SOURCE = """int observe(int& value) { return value; }
int main() { /* 😀 */ return observe(42) - 42; }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-temporary-borrow-lsp-") as directory:
        uri = (Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0
        version = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply.get("result")

        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("method") == "textDocument/publishDiagnostics" and item["params"].get("uri") == uri and item["params"].get("version") == version)
            return [item for item in reply["params"]["diagnostics"] if item.get("severity") == 1]

        try:
            request("initialize", {"capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert not publish(SOURCE)
            position = {"textDocument": {"uri": uri}, "position": lsp_position(SOURCE, SOURCE.index("observe(42)"))}
            assert "int32_t&" in str(request("textDocument/hover", position))
            assert request("textDocument/definition", position)
            invalid = SOURCE.replace("int& value", "mut int& value")
            diagnostics = publish(invalid)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2017")
            assert "mutable reference to a temporary" in error["message"], error
            at = invalid.index("observe(42)") + len("observe(")
            assert error["range"]["start"] == lsp_position(invalid, at), error
            assert error["range"]["end"] == lsp_position(invalid, at + 2), error
            assert not publish(SOURCE)
            ambiguity = """class Tag { public: Tag() = default; bool operator==(Tag& x) { return true; } bool operator==(Tag x) { return true; } };
int main() { Tag a{}; Tag b{}; return a == b ? 0 : 1; }
"""
            diagnostics = publish(ambiguity)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2022")
            assert len(error.get("relatedInformation", [])) >= 2, error
            assert not publish(SOURCE)
        finally:
            session.close()


if __name__ == "__main__":
    main()
