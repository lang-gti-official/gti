#!/usr/bin/env python3
"""Init statements keep compiler-owned scope, navigation and diagnostics."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, open_document


def main():
    source = """int pick(int input) {
  if (int doubled = input * 2; doubled > 4) {
    return doubled;
  } else if (int tripled = doubled + input; tripled > 6) {
    return tripled;
  } else {
    return doubled + 1;
  }
}
int main() {
  switch (int chosen = pick(2); chosen) {
  default:
    return chosen;
  }
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-init-statement-lsp-") as temporary:
        uri = (pathlib.Path(temporary) / "init.gti").as_uri()
        session = LspSession(sys.argv[1])
        next_id = 10

        def request(method, params):
            nonlocal next_id
            next_id += 1
            request_id = next_id
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": method, "params": params})
            return session.receive_until(
                lambda message: message.get("id") == request_id)["result"]

        def at(text, offset):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, offset)}

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(session, uri, source)

            declaration = source.index("doubled =")
            # The init name resolves from the condition, the then branch, the
            # else-if init and the final else branch.
            uses = [source.index("doubled > 4"),
                    source.index("doubled;"),
                    source.index("doubled + input"),
                    source.index("doubled + 1")]
            for use in uses:
                hover = request("textDocument/hover", at(source, use))
                assert hover and "int32_t" in str(hover), hover
                definition = request("textDocument/definition", at(source, use))
                location = definition[0] if isinstance(definition, list) else definition
                assert location["range"]["start"] == lsp_position(source, declaration), \
                    (use, definition)
            references = request("textDocument/references", {
                **at(source, declaration), "context": {"includeDeclaration": True}})
            assert len(references) == len(uses) + 1, references
            switch_use = source.index("chosen;")
            definition = request("textDocument/definition", at(source, switch_use))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == lsp_position(
                source, source.index("chosen =")), definition

            # A use after the statement is outside the init's scope.
            invalid = source.replace("return chosen;\n  }\n}",
                                     "return chosen;\n  }\n  return chosen;\n}")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": invalid}]}})
            diagnostics = session.receive_until(
                lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 2)["params"]["diagnostics"]
            undefined = [item for item in diagnostics if item.get("code") == "GTI-S2001"]
            assert len(undefined) == 1, diagnostics
            outside = invalid.rindex("chosen;")
            assert undefined[0]["range"]["start"] == lsp_position(invalid, outside), undefined
            assert undefined[0]["range"]["end"] == lsp_position(
                invalid, outside + len("chosen")), undefined

            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 3},
                                     "contentChanges": [{"text": source}]}})
            recovered = session.receive_until(
                lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 3)["params"]["diagnostics"]
            assert recovered == [], recovered

            formatted = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            assert formatted == [], formatted
        finally:
            session.close()
    print("Init statement LSP checks passed")


if __name__ == "__main__":
    main()
