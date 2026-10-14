#!/usr/bin/env python3
"""Access diagnostics and receiver-aware completions come from the frontend."""
from pathlib import Path
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position

BASE = """// UTF-16: 😀
class Base {
protected:
  int secret = 7;
  int helper() { return secret; }
public:
  Base() {}
  int read() { return secret; }
};
struct Sibling : public Base {};
"""

def main():
    with tempfile.TemporaryDirectory(prefix="gti-protected-lsp-") as directory:
        uri = (Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        seq, version = 0, 0
        def request(method, params):
            nonlocal seq
            seq += 1
            session.send({"jsonrpc": "2.0", "id": seq, "method": method, "params": params})
            reply = session.receive_until(lambda x: x.get("id") == seq)
            assert "error" not in reply, reply
            return reply["result"]
        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                    "textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}})
            else:
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
            return session.receive_until(lambda x: x.get("method") == "textDocument/publishDiagnostics"
                and x["params"].get("uri") == uri and x["params"].get("version") == version)["params"]["diagnostics"]
        def at(source, offset):
            return {"textDocument": {"uri": uri}, "position": lsp_position(source, offset)}
        try:
            init = request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            legend = init["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            valid = BASE + "struct Derived : public Base { int get(Derived& peer) { return peer.secret; } };\n"
            assert not publish(valid)
            use = valid.rindex("secret")
            hover = request("textDocument/hover", at(valid, use))
            assert "int32_t" in str(hover), hover
            definition = request("textDocument/definition", at(valid, use))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"] == lsp_position(valid, valid.index("secret")), definition
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"])
            pos = lsp_position(valid, valid.index("protected"))
            assert legend[tokens[(pos["line"], pos["character"])]["type"]] == "keyword"
            for receiver, allowed in (("Derived", True), ("Base", False), ("Sibling", False)):
                for prefix in ("sec", "hel"):
                    source = BASE + f"struct Derived : public Base {{ int get({receiver}& peer) {{ return peer.{prefix}; }} }};\n"
                    publish(source)
                    result = request("textDocument/completion", at(source, source.rindex(prefix) + len(prefix)))
                    items = result["items"] if isinstance(result, dict) else result
                    names = {item["label"] for item in items}
                    expected = "secret" if prefix == "sec" else "helper"
                    assert (expected in names) == allowed, (receiver, prefix, names)
            denied = BASE + 'int read(Base& value) { /* 😀 */ return value.secret; }\n'
            errors = publish(denied)
            error = next(item for item in errors if item["code"] == "GTI-S2007")
            assert "protected" in error["message"]
            assert error["range"]["start"] == lsp_position(denied, denied.rindex("secret")), error
            related = error["relatedInformation"][0]["location"]
            assert related["uri"] == uri and related["range"]["start"] == lsp_position(denied, denied.index("secret")), related
            assert not publish(denied.replace("value.secret", "value.read()"))
            assert not publish(valid)
        finally:
            session.close()
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
