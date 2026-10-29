#!/usr/bin/env python3
"""Restoring APIs retain compiler-owned constraints and editor recovery."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import without_include_advice, LspSession, lsp_position


def main():
    source = """#include <std/utility>
#include <std/vector>
class Resource {
public:
  Resource() {}
  Resource(Resource& other) = delete;
};
int main() {
  mut Resource first{};
  mut Resource second{};
  /*😀*/ std::swap(first, second);
  Resource old = std::exchange(first, Resource());
  mut std::vector<Resource> values{};
  values.push_back(std::move(old));
  values.swap_elements(std::size_t(0), std::size_t(0));
  return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-restoring-lsp-") as directory:
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
            return without_include_advice(reply["params"]["diagnostics"])

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            assert not diagnostics(1)
            for name, header in (("swap", "utility"), ("exchange", "utility"),
                                 ("swap_elements", "vector")):
                offset = source.index(name + "(")
                position = {"textDocument": {"uri": uri},
                            "position": lsp_position(source, offset + 2)}
                hover = request("textDocument/hover", position)
                assert name in str(hover) and "relocatable" in str(hover), hover
                definition = request("textDocument/definition", position)
                location = definition[0] if isinstance(definition, list) else definition
                assert location["uri"].endswith(f"/stdlib/std/{header}.gti"), definition

            # A deleted generated move cannot become legal through a wrapper.
            invalid = source.replace("Resource() {}", "Resource() {} Resource(Resource&& other) = delete;")
            change(2, invalid)
            errors = diagnostics(2)
            constraint = next(item for item in errors
                              if item.get("code") == "GTI-S2029"
                              and "relocatable" in item["message"])
            assert constraint.get("relatedInformation"), constraint
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors

            # Self-swap is an overlapping exclusive loan, not an unchecked no-op.
            invalid = source.replace("swap(first, second)", "swap(first, first)")
            change(3, invalid)
            errors = diagnostics(3)
            assert errors and not any(item.get("code") == "GTI-B0001" for item in errors), errors
            close = invalid.index(")", invalid.index("std::swap"))
            alias = next(item for item in errors if item.get("code") == "GTI-S2017")
            assert "overlapping reference accesses" in alias["message"], alias
            assert alias["range"] == {
                "start": lsp_position(invalid, close),
                "end": lsp_position(invalid, close + 1)}, alias
            change(4, source.replace("std::exchange(first, Resource())", "std::exchange("))
            assert diagnostics(4)
            change(5, source)
            assert not diagnostics(5)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            change(6, formatted)
            assert not diagnostics(6)
            print("restoring exchange LSP: passed")
        finally:
            session.close()


if __name__ == "__main__":
    main()
