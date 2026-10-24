#!/usr/bin/env python3
"""Thread-local storage facts survive editor queries and unsaved edits."""
from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position

SOURCE = """thread_local mut int counter = 0;
int main() { /* 😀 */ counter += 1; return counter; }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-thread-local-lsp-") as directory:
        uri = (Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0
        version = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            return session.receive_until(lambda item: item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri and item["params"].get("version") == version)["params"]["diagnostics"]

        def at(source, offset):
            return {"textDocument": {"uri": uri}, "position": lsp_position(source, offset)}

        try:
            initialized = request("initialize", {"capabilities": {}})
            legend = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert not publish(SOURCE)
            use = SOURCE.rindex("counter")
            hover = request("textDocument/hover", at(SOURCE, use))
            assert "thread_local" in str(hover) and "counter" in str(hover), hover
            definition = request("textDocument/definition", at(SOURCE, use))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(SOURCE, SOURCE.index("counter")), definition
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"])
            keyword = lsp_position(SOURCE, SOURCE.index("thread_local"))
            assert legend[tokens[(keyword["line"], keyword["character"])]["type"]] == "keyword", tokens

            ordinary = SOURCE.replace("thread_local ", "")
            assert not publish(ordinary)
            hover = request("textDocument/hover", at(ordinary, ordinary.rindex("counter")))
            assert "thread_local" not in str(hover), hover

            invalid = "// 😀\nthread_local int invalid() { return 0; }\n"
            diagnostics = publish(invalid)
            assert any(item.get("code") == "GTI-P0001" and
                       item["range"]["start"] == lsp_position(invalid, invalid.index("thread_local"))
                       for item in diagnostics), diagnostics
            assert not publish(SOURCE)
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
