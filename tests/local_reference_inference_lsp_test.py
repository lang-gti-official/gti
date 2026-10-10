#!/usr/bin/env python3
"""Inferred references use current compiler types, access and dependency facts."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


OWNER = """struct Record {
  mut int count = 1;
  Record() = default;
  Record(Record& other) = delete;
  Record(Record&& other) = default;
};
"""
SOURCE = """#include "owner.gti"
int main() {
  mut Record owner{};
  /* 😀 */ auto& observed = owner;
  int snapshot = observed.count;
  mut auto& writable = owner;
  writable.count = snapshot + 1;
  return writable.count - 2;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-local-reference-lsp-") as temporary:
        directory = pathlib.Path(temporary)
        path = directory / "main.gti"
        dependency = directory / "owner.gti"
        path.write_text(SOURCE)
        dependency.write_text(OWNER)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def publication(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def open_document(target, source, version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": target.as_uri(), "languageId": "gti", "version": version, "text": source}}})

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
            return publication(version)

        def position(fragment):
            return {"textDocument": {"uri": uri}, "position": lsp_position(SOURCE, SOURCE.index(fragment))}

        def clean(diagnostics):
            assert not [item for item in diagnostics if item.get("severity", 1) == 1], diagnostics

        try:
            request("initialize", {"capabilities": {"textDocument": {"publishDiagnostics": {
                "relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(path, SOURCE, 1)
            clean(publication(1))
            shared = request("textDocument/hover", position("auto& observed"))
            exclusive = request("textDocument/hover", position("auto& writable"))
            assert "Record&" in shared["contents"]["value"], shared
            assert "mut Record&" in exclusive["contents"]["value"], exclusive
            definitions = request("textDocument/definition", position("observed.count"))
            definition = definitions[0] if isinstance(definitions, list) else definitions
            assert definition["uri"] == uri, definition
            assert definition["range"]["start"] == lsp_position(SOURCE, SOURCE.index("observed =")), definition

            copied = SOURCE.replace("auto& observed", "auto observed")
            diagnostics = change(2, copied)
            error = next(item for item in diagnostics if item.get("code") == "GTI-S2003")
            assert "auto&" in error["message"] and "clone" in error["message"], error
            offset = copied.index("owner;", copied.index("auto observed"))
            assert error["range"] == {"start": lsp_position(copied, offset),
                                       "end": lsp_position(copied, offset + 5)}, error
            clean(change(3, SOURCE))
            conflict = SOURCE.replace("int snapshot = observed.count;", "owner.count = 5; int snapshot = observed.count;")
            assert any(item.get("code") == "GTI-S2017" for item in change(4, conflict))
            readonly = SOURCE.replace("mut Record owner", "Record owner")
            assert any(item.get("code") == "GTI-S2017" for item in change(5, readonly))
            broken = SOURCE.replace("auto& observed = owner;", "auto& observed = ;")
            assert change(6, broken)
            clean(change(7, SOURCE))
            assert request("textDocument/hover", position("auto& observed")) == shared

            # An unsaved dependency must change the referent's field access in
            # this root, not just diagnostics in the dependency's own buffer.
            open_document(dependency, OWNER.replace("mut int count", "int count"), 1)
            assert any(item.get("severity", 1) == 1 for item in publication(7))
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {
                "textDocument": {"uri": dependency.as_uri()}}})
            clean(publication(7))
            unformatted = (SOURCE.replace("auto& observed = owner", "auto&observed=owner")
                           .replace("mut auto& writable", "mut auto &writable")
                           .replace("  int snapshot = observed.count", "int snapshot=observed.count"))
            clean(change(8, unformatted))
            edits = request("textDocument/formatting", {"textDocument": {"uri": uri},
                            "options": {"tabSize": 2, "insertSpaces": True}})
            assert edits and edits[0]["newText"] == SOURCE, edits
            clean(change(9, edits[0]["newText"]))
        finally:
            session.close()
    print("Inferred reference LSP checks passed")


if __name__ == "__main__":
    main()
