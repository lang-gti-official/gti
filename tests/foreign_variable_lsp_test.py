#!/usr/bin/env python3
"""Foreign storage tooling consumes compiler identities and access facts."""
from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position

SOURCE = '''// Foreign storage 😀
extern "C" {
  [[link_name("native_counter")]] mut c_int counter;
  c_int limit;
}
c_int read() { unsafe { return counter; } }
'''


def main():
    with tempfile.TemporaryDirectory(prefix="gti-foreign-storage-lsp-") as directory:
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
            signature = str(hover)
            assert 'extern "C"' in signature and ("mut c_int counter" in signature or "mut int32_t counter" in signature), hover
            assert "native_counter" in signature and "unsafe" in signature, hover
            read_only = request("textDocument/hover", at(SOURCE, SOURCE.index("limit")))
            assert " limit" in str(read_only) and "mut " not in str(read_only), read_only
            definition = request("textDocument/definition", at(SOURCE, use))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(SOURCE, SOURCE.index("counter;")), definition
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"])
            position = lsp_position(SOURCE, use)
            assert legend[tokens[(position["line"], position["character"])]["type"]] == "variable", tokens
            invalid = SOURCE.replace("unsafe { return counter; }", "/* 😀 */ return counter;")
            diagnostics = publish(invalid)
            assert any("unsafe" in item["message"] and item["range"]["start"] ==
                       lsp_position(invalid, invalid.rindex("counter")) for item in diagnostics), diagnostics
            assert not publish(SOURCE)
            edited = SOURCE.replace("native_counter", "renamed_counter")
            assert not publish(edited)
            new_hover = request("textDocument/hover", at(edited, edited.rindex("counter")))
            assert "renamed_counter" in str(new_hover) and "native_counter" not in str(new_hover), new_hover
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
