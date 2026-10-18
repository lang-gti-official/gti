#!/usr/bin/env python3
"""Callable-result query errors have stable ranges and recover on editing."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = '''#include <std/meta>
/*😀*/ using Result = std::meta::callable_return_type<int>;
int main() { return 0; }
'''
    with tempfile.TemporaryDirectory(prefix="gti-callable-result-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            assert "error" not in session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda item:
                    item.get("method") == "textDocument/publishDiagnostics"
                    and item["params"].get("uri") == uri
                    and item["params"].get("version") == version)["params"]["diagnostics"]

            errors = diagnostics(1)
            error = next(item for item in errors if item.get("code") == "GTI-S2023")
            start = source.index("callable_return_type")
            assert error["range"] == {"start": lsp_position(source, start),
                                      "end": lsp_position(source, start + len("callable_return_type"))}, error
            assert "does not select an overloaded operator()" in error["message"], error
            valid = source.replace("callable_return_type<int>", "callable_return_type<int()>")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert not diagnostics(2)
        finally:
            session.close()


if __name__ == "__main__":
    main()
