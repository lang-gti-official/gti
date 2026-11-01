#!/usr/bin/env python3
"""Immediate declarations retain editor identity and required-call diagnostics."""

from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


SOURCE = """consteval int square(int value) { return value * value; }
struct Port {
  int value;
  consteval Port(int number) : value(number) {}
};
int use(int runtime) { /* 😀 */ return square(3) + Port(80).value + runtime; }
int main() { return use(0) - 89; }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-consteval-lsp-") as directory:
        uri = (Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0
        version = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti",
                                           "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version},
                          "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            message = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)
            return message["params"]["diagnostics"]

        def at(source, offset):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(source, offset)}

        try:
            initialized = request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            legend = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert not publish(SOURCE)

            for spelling in ("square(3)", "Port(80)"):
                hover = request("textDocument/hover", at(SOURCE, SOURCE.index(spelling)))
                assert "consteval" in str(hover) and spelling.split("(")[0] in str(hover), hover
            definition = request("textDocument/definition", at(SOURCE, SOURCE.index("square(3)")))
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(SOURCE, SOURCE.index("square")), definition
            signature = request("textDocument/signatureHelp", at(SOURCE, SOURCE.index("square(3)") + len("square(")))
            assert "consteval" in str(signature) and "square" in str(signature), signature

            tokens = semantic_tokens_by_position(request("textDocument/semanticTokens/full",
                {"textDocument": {"uri": uri}})["data"])
            for offset in (SOURCE.index("consteval"), SOURCE.index("consteval Port")):
                position = lsp_position(SOURCE, offset)
                token = tokens[(position["line"], position["character"])]
                assert legend[token["type"]] == "keyword" and token["length"] == len("consteval"), token

            invalid = SOURCE.replace("square(3)", "square(runtime)")
            diagnostics = publish(invalid)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2057")
            assert "Consteval invocation" in error["message"], error
            call_end = invalid.index("square(runtime)") + len("square(runtime")
            assert error["range"] == {"start": lsp_position(invalid, call_end),
                                      "end": lsp_position(invalid, call_end + 1)}, error
            assert any("Immediate declaration" in item["message"]
                       for item in error["relatedInformation"]), error
            assert any("runtime" in item["message"]
                       for item in error["relatedInformation"]), error

            ordinary = invalid.replace("consteval int square", "constexpr int square")
            assert not publish(ordinary)
            hover = request("textDocument/hover", at(ordinary, ordinary.index("square(runtime)")))
            assert "constexpr" in str(hover) and "consteval" not in str(hover), hover

            statement = """consteval void validate(int value) { int checked = 100 / value; }
struct Default { int value; implicit consteval Default() : value(42) {} };
Default make() { return; }
int main() { validate(2); return make().value - 42; }
"""
            assert not publish(statement)
            invalid_void = statement.replace("validate(2)", "validate(0)")
            diagnostics = publish(invalid_void)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2057")
            assert any("failed at this operation" in item["message"]
                       for item in error["relatedInformation"]), error
            assert any("Immediate declaration" in item["message"]
                       for item in error["relatedInformation"]), error
            invalid_return = statement.replace("value(42)", "value(1 / 0)")
            diagnostics = publish(invalid_return)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2057")
            assert "return" in invalid_return.splitlines()[error["range"]["start"]["line"]], error
            assert any("Immediate declaration" in item["message"]
                       for item in error["relatedInformation"]), error

            incomplete = "consteval int pending(int value"
            assert publish(incomplete)
            assert not publish(SOURCE)
        finally:
            session.close()
    print("consteval LSP signatures, tokens, navigation and diagnostic edits passed")


if __name__ == "__main__":
    main()
