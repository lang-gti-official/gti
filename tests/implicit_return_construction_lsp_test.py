#!/usr/bin/env python3
"""Declared return initialization follows current compiler-owned overlays."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, has_error_diagnostics, lsp_position

SOURCE = """struct Result {
  int value;
  implicit Result(int input) : value(input) {}
};
Result make(int value) { /* 😀 */ return value; }
int main() { return make(7).value; }
"""

def main():
    with tempfile.TemporaryDirectory(prefix="gti-return-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                     "version": 1, "text": SOURCE}}})
            def publication(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics"
                    and message["params"]["uri"] == uri
                    and message["params"].get("version") == version)["params"]

            def change(source, version):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                              "params": {"textDocument": {"uri": uri, "version": version},
                                         "contentChanges": [{"text": source}]}})
                return publication(version)

            assert not has_error_diagnostics(publication(1))
            offset = SOURCE.index("value;", SOURCE.index("Result make"))
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(SOURCE, offset)}})
            location = session.receive_until(lambda message: message.get("id") == 2)["result"]
            location = location[0] if isinstance(location, list) else location
            assert location["uri"] == uri, location
            assert location["range"]["start"] == lsp_position(
                SOURCE, SOURCE.index("value)", SOURCE.index("Result make"))), location

            explicit = SOURCE.replace("implicit Result", "Result")
            diagnostics = change(explicit, 2)["diagnostics"]
            mismatch = next(d for d in diagnostics if d.get("code") == "GTI-S2003")
            offset = explicit.index("value;", explicit.index("Result make"))
            assert mismatch["range"]["start"] == lsp_position(explicit, offset), mismatch
            assert not has_error_diagnostics(change(SOURCE, 3))
            broken = SOURCE.replace("value(input)", "value(input")
            assert has_error_diagnostics(change(broken, 4))
            assert not has_error_diagnostics(change(SOURCE, 5))
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(SOURCE, SOURCE.index("make(int"))}})
            hover = session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert "Result" in str(hover) and "make" in str(hover), hover
        finally:
            session.close()
    print("Implicit return overlays, exact diagnostics and UTF-16 navigation passed")

if __name__ == "__main__":
    main()
