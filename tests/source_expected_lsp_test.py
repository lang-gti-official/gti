#!/usr/bin/env python3
"""Production result names are ordinary source declarations in editor queries."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


SOURCE = """enum Error { missing };
expected<int, Error> read(bool available) {
  if (available) { return 7; }
  return unexpected(Error::missing);
}
int ordinary_names() {
  int expected = 3;
  int unexpected = 4;
  return expected + unexpected;
}
int main() {
  auto result = read(true);
  return result.has_value() ? result.value() : ordinary_names();
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-source-expected-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(SOURCE)
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
            return diagnostics(version)

        def position(text, spelling):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, text.index(spelling))}

        try:
            initialized = request("initialize", {"capabilities": {}})
            legend = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": SOURCE}}})
            assert diagnostics(1) == []
            for spelling in ("expected<int", "unexpected(Error"):
                params = position(SOURCE, spelling)
                hover = request("textDocument/hover", params)
                assert hover and spelling.split("(")[0].split("<")[0] in str(hover), hover
                definitions = request("textDocument/definition", params)
                if isinstance(definitions, dict):
                    definitions = [definitions]
                assert definitions and any(
                    item.get("uri", item.get("targetUri", "")).endswith("/prelude.gti")
                    for item in definitions), definitions
            tokens = semantic_tokens_by_position(request(
                "textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"])
            for spelling, accepted in (("expected<int", {"class", "type"}),
                                       ("unexpected(Error", {"function"})):
                pos = position(SOURCE, spelling)["position"]
                token = tokens[(pos["line"], pos["character"])]
                assert legend[token["type"]] in accepted, (spelling, token, legend)

            incomplete = SOURCE.replace("result.has_value() ? result.value() : ordinary_names()", "result.")
            assert change(2, incomplete)
            pos = position(incomplete, "result.;")
            pos["position"]["character"] += len("result.")
            completion = request("textDocument/completion", pos)
            items = completion["items"] if isinstance(completion, dict) else completion
            assert {"value", "has_value", "transform_error", "and_then"} <= {
                item["label"] for item in items}, items
            assert change(3, SOURCE) == []
            assert request("textDocument/hover", position(SOURCE, "unexpected(Error"))
        finally:
            session.close()


if __name__ == "__main__":
    main()
