#!/usr/bin/env python3
"""Borrowed callable contracts and conditional instance errors share the frontend."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """class Counter {
public:
  mut int count = 0;
  int operator()(int n) mut { this.count += n; return this.count; }
};
int pick<F>(mut F& producer, bool ok) {
  auto result = ok ? producer(1) : 2;
  return result;
}
int main() {
  mut Counter counter{};
  /* 😀 */ return pick(counter, true);
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-borrowed-callable-lsp-") as temporary:
        path = pathlib.Path(temporary) / "main.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def publication(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
            return publication(version)

        def clean(diagnostics):
            assert not [item for item in diagnostics if item.get("severity", 1) == 1], diagnostics

        try:
            request("initialize", {"capabilities": {"textDocument": {"publishDiagnostics": {
                "relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": SOURCE}}})
            clean(publication(1))
            position = {"textDocument": {"uri": uri},
                        "position": lsp_position(SOURCE, SOURCE.index("pick(counter"))}
            hover = request("textDocument/hover", position)
            assert "mut Counter&" in hover["contents"]["value"], hover
            definition = request("textDocument/definition", position)
            definition = definition[0] if isinstance(definition, list) else definition
            assert definition["uri"] == uri, definition
            assert definition["range"]["start"] == lsp_position(SOURCE, SOURCE.index("pick<F>")), definition
            mismatch = SOURCE.replace("producer(1) : 2", "producer(1) : false")
            diagnostics = change(2, mismatch)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2050")
            assert "same exact type" in error["message"], error
            assert not any(item.get("code") == "GTI-B0001" for item in diagnostics), diagnostics
            clean(change(3, SOURCE))
            readonly = SOURCE.replace("mut F& producer", "F& producer")
            diagnostics = change(4, readonly)
            assert any("read-callable" in item["message"] for item in diagnostics), diagnostics
            incomplete = SOURCE.replace("producer(1) : 2", "producer(1) :")
            assert change(5, incomplete)
            clean(change(6, SOURCE))
            assert request("textDocument/hover", position) == hover
        finally:
            session.close()
    print("Borrowed callable and conditional LSP checks passed")


if __name__ == "__main__":
    main()
