#!/usr/bin/env python3
"""Owned range bindings retain editor identities and recover after invalid edits."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position


def main():
    source = """import std.vector;
import std.string;
int main() {
  mut std::vector<std::string> values{};
  values.emplace_back("owned");
  mut std::size_t size = 0;
  for (auto word : std::move(values).into_range()) {
    size += word.size();
  }
  return size == 5 ? 0 : 1;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-consuming-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
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
            reply = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)
            return reply["params"]["diagnostics"]
        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})
        try:
            request("initialize", {"capabilities": {}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            assert not diagnostics(1)
            word = {"textDocument": {"uri": uri},
                    "position": lsp_position(source, source.index("word.size") + 2)}
            hover = request("textDocument/hover", word)
            assert "string" in str(hover), hover
            definition = request("textDocument/definition", word)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(source, source.index("word :")), definition
            references = request("textDocument/references", dict(word, context={"includeDeclaration": True}))
            expected = [lsp_position(source, source.index("word :")),
                        lsp_position(source, source.index("word.size"))]
            assert [ref["range"]["start"] for ref in references] == expected, references
            renamed = request("textDocument/rename", dict(word, newName="owned_word"))
            assert str(renamed).count("owned_word") == 2, renamed
            method = {"textDocument": {"uri": uri},
                      "position": lsp_position(source, source.index("into_range") + 3)}
            definition = request("textDocument/definition", method)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"].endswith("/stdlib/std/vector.gti"), definition
            change(2, source.replace("return size == 5", "return values.empty() && size == 5"))
            errors = diagnostics(2)
            assert errors and not any(item.get("code") == "GTI-B0001" for item in errors), errors
            assert any("moved" in item["message"].lower() for item in errors), errors
            change(3, source)
            assert not diagnostics(3)
            change(4, source.replace("auto word :", "auto& word :"))
            errors = diagnostics(4)
            assert any(item.get("code") == "GTI-S2083" for item in errors), errors
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors
            change(5, source)
            assert not diagnostics(5)
        finally:
            session.close()


if __name__ == "__main__":
    main()
