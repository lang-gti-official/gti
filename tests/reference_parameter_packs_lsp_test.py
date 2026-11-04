#!/usr/bin/env python3
"""Reference-pack syntax, access, navigation and diagnostics share the frontend."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = (
        "void observe<Args...>(Args&... args) {}\n"
        "void change<Args...>(mut Args&... args) {}\n"
        "int main() { mut int number = 4; observe(number, number); change(number); return number; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-reference-packs-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
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
            capabilities = request("initialize", {"capabilities": {}})
            legend = capabilities["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            assert not diagnostics(1)
            params = {"textDocument": {"uri": uri},
                      "position": lsp_position(source, source.index("observe(number"))}
            hover = request("textDocument/hover", params)
            assert "observe" in str(hover) and "&" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"]["line"] == 0, definition
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            pos = lsp_position(source, source.index("args"))
            assert legend[tokens[(pos["line"], pos["character"])]["type"]] == "parameter", tokens
            change(2, source.replace("change(number)", "change(number, number)"))
            assert any(item.get("code") == "GTI-S2017" for item in diagnostics(2))
            change(3, source)
            assert not diagnostics(3)
            edits = request("textDocument/formatting", {"textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            change(4, formatted)
            assert not diagnostics(4)
        finally:
            session.close()
    print("reference parameter packs: LSP navigation, access, tokens, formatting and recovery passed")


if __name__ == "__main__":
    main()
