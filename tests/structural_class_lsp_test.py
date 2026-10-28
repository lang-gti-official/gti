#!/usr/bin/env python3
"""Class annotations use compiler concept identities and diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    valid = """// μ: a non-ASCII prefix must not disturb diagnostic locations.
concept readable<T> = requires(T& source) { { source.read() } -> int; };
struct Meter { Meter() = default; int read() { return 7; } };
class Snapshot<readable T> {
  T& source;
public:
  Snapshot(T& source) : source(source) {}
  int read() { return this.source.read(); }
  ~Snapshot() { int final_sample = this.source.read(); }
};
int main() { Meter meter{}; Snapshot<Meter> value{meter}; return value.read()-7; }
"""
    with tempfile.TemporaryDirectory(prefix="gti-class-concept-lsp-") as directory:
        path = pathlib.Path(directory) / "concept.gti"
        path.write_text(valid)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": valid}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            assert not diagnostics(1)
            annotation = valid.index("readable T")
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, annotation + 2)}})
            definition = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert definition and "concept.gti" in str(definition), definition
            invalid = valid.replace("int read() { return 7; }", "bool read() { return true; }")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": invalid}]}})
            errors = diagnostics(2)
            error = next(item for item in errors if item.get("code") == "GTI-S2029")
            assert "readable" in error["message"], error
            application = invalid.index("Snapshot<Meter>")
            assert error["range"]["start"]["line"] == lsp_position(invalid, application)["line"], error
            assert any("read" in item["message"] for item in error["relatedInformation"]), error
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": valid}]}})
            assert not diagnostics(3)
        finally:
            session.close()
    print("structural class concept LSP navigation and diagnostics passed")


if __name__ == "__main__":
    main()
