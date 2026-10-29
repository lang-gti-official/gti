#!/usr/bin/env python3
"""The optional method contract is published at the call, with recovery."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import without_include_advice, LspSession, lsp_position


def main():
    source = '''#include <std/optional>
struct Resource {
  Resource() {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
};
int main() {
  std::optional<Resource> value{};
  /*😀*/ auto result = value.value_or(Resource());
  return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="gti-optional-contract-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
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
            return without_include_advice(session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"])

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": source}}})
            errors = diagnostics(1)
            error = next(item for item in errors if item.get("code") == "GTI-S2029")
            offset = source.index("));") + 1
            assert error["range"] == {"start": lsp_position(source, offset),
                                      "end": lsp_position(source, offset + 1)}, error
            assert "std::copy_constructible" in error["message"] and "Resource" in error["message"], error
            related = error.get("relatedInformation", [])
            assert any("Required application 'std::copy_constructible<Resource>'" in item["message"] for item in related), error
            assert any("optional" in item["location"]["uri"] for item in related), error
            assert any("copy assignment is not required" in hint
                       for hint in error.get("data", {}).get("hints", [])), error
            fixes = request("textDocument/codeAction", {"textDocument": {"uri": uri},
                "range": error["range"], "context": {"diagnostics": [error]}})
            assert not fixes, fixes

            # A contract error must not prevent other member completion.
            incomplete = source.replace("value_or(Resource())", "")
            change(2, incomplete)
            assert diagnostics(2)
            offset = incomplete.index("value.;") + len("value.")
            completion = request("textDocument/completion", {"textDocument": {"uri": uri},
                "position": lsp_position(incomplete, offset)})
            items = completion["items"] if isinstance(completion, dict) else completion
            assert any(item["label"] == "has_value" for item in items), items
            valid = source.replace("auto result = value.value_or(Resource());",
                                   "bool result = value.has_value();")
            change(3, valid)
            assert not diagnostics(3)
            promoted = '''import std;
std::optional<int> find(bool available) {
  if (available) { return std::optional<int>(7); }
  std::optional<int> none = std::nullopt;
  return none;
}
std::optional<int> relay(bool available) {
  int value = ?find(available);
  return std::optional<int>(value + 1);
}
int main() {
  mut std::optional<int> value = relay(true);
  value = relay(false);
  return value.has_value() ? 1 : 0;
}
'''
            change(4, promoted)
            assert not diagnostics(4)
            broken = promoted.replace("return value.has_value() ? 1 : 0;",
                                      "return value.;")
            change(5, broken)
            assert diagnostics(5)
            offset = broken.index("value.;") + len("value.")
            completion = request("textDocument/completion", {
                "textDocument": {"uri": uri},
                "position": lsp_position(broken, offset)})
            items = completion["items"] if isinstance(completion, dict) else completion
            assert {"value", "has_value", "reset"} <= {
                item["label"] for item in items}, items
            change(6, promoted)
            assert not diagnostics(6)
        finally:
            session.close()


if __name__ == "__main__":
    main()
