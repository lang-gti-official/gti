#!/usr/bin/env python3
"""Indexed owner relays retain diagnostics and navigation across edits."""

import os
from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    repository = Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    fixture = (repository / "tests/fixtures/indexed_view_relay.gti").read_text()
    prefix = fixture[:fixture.index("int main()")]
    source = prefix + """int main() {
  mut Records records{};
  std::string_view held = records.at(std::size_t(0));
  // edit here
  std::println(held);
  return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-operator-relay-lsp-") as temporary:
        path = Path(temporary) / "main.gti"
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
            return reply.get("result")

        def errors(version):
            publication = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)
            return [item for item in publication["params"]["diagnostics"]
                    if item.get("severity") == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            assert not errors(1)
            position = {"textDocument": {"uri": uri}, "position":
                        lsp_position(source, source.index("records.at") + len("records."))}
            assert "string_view" in str(request("textDocument/hover", position))
            assert request("textDocument/definition", position)
            edited = source.replace("// edit here", "/*😀*/ records.add();")
            change(2, edited)
            diagnostics = errors(2)
            assert diagnostics and all(item.get("code") != "GTI-B0001"
                                       for item in diagnostics), diagnostics
            assert any(item.get("code") == "GTI-S2017" for item in diagnostics), diagnostics
            conflict = next(item for item in diagnostics if item.get("code") == "GTI-S2017")
            assert conflict["range"]["start"] == lsp_position(
                edited, edited.index("records.add()") + len("records.add(")), conflict
            assert conflict.get("relatedInformation"), conflict
            assert "string_view" in str(request("textDocument/hover", position))
            change(3, source)
            assert not errors(3)
            assert request("textDocument/definition", position)
        finally:
            session.close()


if __name__ == "__main__":
    main()
