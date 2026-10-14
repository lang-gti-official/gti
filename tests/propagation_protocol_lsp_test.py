#!/usr/bin/env python3
"""Protocol propagation through the editor: clean, rejected and recovered."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    source = (root / "examples/156-propagation-protocol.gti").read_text()
    # Returning another family from a propagating function is rejected at `?`.
    broken = source.replace("std::optional<int> distance_squared(",
                            "Loaded distance_squared(", 1)
    assert broken != source
    with tempfile.TemporaryDirectory(prefix="gti-propagation-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": source}}})
            clean = [item for item in diagnostics(1) if item.get("severity") == 1]
            assert clean == [], clean

            change(2, broken)
            errors = diagnostics(2)
            error = next(item for item in errors if item.get("code") == "GTI-S2086")
            assert "returning 'std::optional'" in error["message"], error
            assert "this one returns 'Loaded'" in error["message"], error
            offset = broken.index("Point q = ?p;") + len("Point q = ")
            start = lsp_position(broken, offset)
            assert error["range"]["start"] == start, (error, start)
            assert error["range"]["end"] == lsp_position(broken, offset + 1), error

            change(3, source)
            recovered = [item for item in diagnostics(3) if item.get("severity") == 1]
            assert recovered == [], recovered
        finally:
            session.close()


if __name__ == "__main__":
    main()
