#!/usr/bin/env python3
"""Reflected invocation diagnostics retain ranges and recover on editing."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = """import std.meta;
class Probe {
public:
  Probe() = default;
  static int32_t value() { return 1; }
  int32_t read() { return 2; }
};
int main() {
  Probe probe{};
  /*😀*/ return std::meta::method<Probe, 0>().invoke(probe);
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-method-reflection-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True, "dataSupport": True}}}}})
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
            error = next(item for item in errors if item.get("code") == "GTI-S2125")
            start = source.index("invoke(probe)") + len("invoke(probe")
            assert error["range"] == {"start": lsp_position(source, start),
                                      "end": lsp_position(source, start + 1)}, error
            assert error["severity"] == 1 and error["data"]["phase"] == "semantics", error
            declaration = source.index("value()")
            assert any(item["location"]["uri"] == uri and
                       item["location"]["range"]["start"] == lsp_position(source, declaration)
                       for item in error["relatedInformation"]), error
            valid = source.replace("method<Probe, 0>", "method<Probe, 1>")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert not diagnostics(2)
        finally:
            session.close()


if __name__ == "__main__":
    main()
