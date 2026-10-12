#!/usr/bin/env python3
"""Named nested types retain compiler-owned editor identities and access."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    source = (
        "// UTF-16 provenance: \U0001f600\n"
        "namespace demo {\n"
        "struct Config {\n"
        "  struct Network { struct Tls { bool enabled = false; }; };\n"
        "  class Logging { public: int level = 1; };\n"
        "  using Selected = Network;\n"
        "private:\n"
        "  struct Secrets { struct Token { int code = 1; }; };\n"
        "};\n"
        "}\n"
        "using Net = demo::Config::Network;\n"
        "using Secure = demo::Config::Network::Tls;\n"
        "int main() { return 0; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-nested-types-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params, expect_error=False):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            if expect_error:
                assert "error" in reply, reply
                return reply["error"]
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            reply = session.receive_until(
                lambda item: item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version
            )
            return reply["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def at(text, offset):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, offset)}

        def completion(text, offset):
            result = request("textDocument/completion", at(text, offset))
            return result["items"] if isinstance(result, dict) else result

        try:
            initialized = request("initialize", {"capabilities": {
                "textDocument": {
                    "publishDiagnostics": {"relatedInformation": True},
                    "documentSymbol": {"hierarchicalDocumentSymbolSupport": True},
                }
            }})
            legend = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                       "version": 1, "text": source}}})
            assert not diagnostics(1)
            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": uri}})["data"])
            for name, declaration in [("Config", "struct Config"),
                                      ("Network", "struct Network"),
                                      ("Tls", "struct Tls")]:
                use = source.rindex(name)
                params = at(source, use)
                hover = request("textDocument/hover", params)
                assert name in str(hover), (name, hover)
                definition = request("textDocument/definition", params)
                location = definition[0] if isinstance(definition, list) else definition
                assert location["uri"] == uri, definition
                expected = source.index(declaration) + len("struct ")
                assert location["range"]["start"] == lsp_position(source, expected), definition
                position = lsp_position(source, use)
                token = tokens[(position["line"], position["character"])]
                assert legend[token["type"]] == "struct", tokens
                references = request("textDocument/references", {
                    **params, "context": {"includeDeclaration": True}})
                assert len(references) == {"Config": 3, "Network": 4, "Tls": 2}[name], references

            outline = request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
            namespace = next(item for item in outline if item["name"] == "demo")
            config = next(item for item in namespace["children"] if item["name"] == "Config")
            network = next(item for item in config["children"] if item["name"] == "Network")
            assert any(item["name"] == "Tls" for item in network["children"]), outline

            # Type identity is complete, but global/type rename must retain the
            # existing fail-closed snapshot boundary rather than partial edits.
            error = request("textDocument/rename", {
                **at(source, source.rindex("Config")), "newName": "Settings"},
                expect_error=True)
            assert error["code"] == -32803 and "function-local" in error["message"], error

            qualified = source.replace("using Net = demo::Config::Network;",
                                       "using Net = demo::Config::;")
            change(2, qualified)
            assert diagnostics(2)
            items = completion(qualified, qualified.index("using Net = demo::Config::") +
                               len("using Net = demo::Config::"))
            names = {item["label"] for item in items}
            assert {"Network", "Logging"} <= names and "Secrets" not in names, items

            inside = source.replace("using Selected = Network;", "using Selected = Netw;")
            change(3, inside)
            assert diagnostics(3)
            items = completion(inside, inside.index("using Selected = Netw") +
                               len("using Selected = Netw"))
            assert any(item["label"] == "Network" for item in items), items

            private = source.replace("demo::Config::Network::Tls",
                                     "demo::Config::Secrets::To")
            change(4, private)
            assert diagnostics(4)
            items = completion(private, private.index("Secrets::To") + len("Secrets::To"))
            assert not any(item["label"] == "Token" for item in items), items

            # Dirty generations replace declarations and their identities together.
            changed = source.replace("Network", "Endpoint")
            change(5, changed)
            assert not diagnostics(5)
            definition = request("textDocument/definition", at(changed, changed.rindex("Endpoint")))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == lsp_position(
                changed, changed.index("struct Endpoint") + len("struct ")), definition
            change(6, source)
            assert not diagnostics(6)
            formatting = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = formatting[0]["newText"] if formatting else source
            assert "struct Network" in formatted and "Config::Network::Tls" in formatted, formatted
            change(7, formatted)
            assert not diagnostics(7)
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
