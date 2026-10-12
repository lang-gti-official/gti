#!/usr/bin/env python3
"""Aggregate imports retain editor queries and recovery on incomplete source."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """import std;
int helper() { return 0; }
int main() {
  /* 😀 */ std::string message{"hello"};
  std::println(message.view());
  return helper();
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-aggregate-lsp-") as temporary:
        path = pathlib.Path(temporary) / "main.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
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

        def publication(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": source}]}})
            return publication(version)

        def clean(diagnostics):
            assert not [d for d in diagnostics if d.get("severity", 1) == 1], diagnostics

        try:
            request("initialize", {"capabilities": {}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": SOURCE}}})
            clean(publication(1))
            position = {"textDocument": {"uri": uri},
                        "position": lsp_position(SOURCE, SOURCE.index("string message"))}
            hover = request("textDocument/hover", position)
            assert "string" in hover["contents"]["value"], hover
            definition = request("textDocument/definition", position)
            definition = definition[0] if isinstance(definition, list) else definition
            assert definition["uri"].endswith("/std/string.gti"), definition
            broken = SOURCE.replace("int helper() { return 0; }", "int helper() {}")
            assert change(2, broken), "missing return should still be diagnosed"
            assert request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": lsp_position(broken, broken.index("string message"))})
            completion_source = broken.replace("std::println(message.view());", "std::str")
            change(3, completion_source)
            completion = request("textDocument/completion", {
                "textDocument": {"uri": uri},
                "position": lsp_position(completion_source,
                    completion_source.rindex("std::str") + len("std::str"))})
            items = completion.get("items", []) if isinstance(completion, dict) else completion
            labels = {item["label"] for item in items}
            assert "string" in labels, labels
            clean(change(4, SOURCE))
            assert request("textDocument/hover", position) == hover
        finally:
            session.close()
    print("Aggregate std import LSP checks passed")


if __name__ == "__main__":
    main()
