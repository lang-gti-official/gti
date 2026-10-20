#!/usr/bin/env python3
"""Borrowed Expected extraction shares compiler lifetime facts and diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """expected<int&, int> lookup(int& owner) { return owner; }
expected<int, int> read(int& owner) {
  int& propagated = ?lookup(owner);
  auto carrier = lookup(owner);
  /*😀*/ int& demanded = std::move(carrier).expect("required");
  return propagated + demanded;
}
int main() {
  mut int owner = 7;
  int& alias = lookup(owner).expect("required");
  int observed = alias;
  owner = 8;
  return observed + owner - 15;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-borrowed-expect-lsp-") as temporary:
        path = pathlib.Path(temporary) / "borrowed.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": source}]}})
            return diagnostics(version)

        def open_document(version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": version, "text": SOURCE}}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(1)
            assert diagnostics(1) == []

            # Expression hover presents the referent type. Navigation still
            # resolves the exact checked reference binding, not its owner.
            offset = SOURCE.index("demanded;")
            params = {"textDocument": {"uri": uri}, "position": lsp_position(SOURCE, offset)}
            hover = request("textDocument/hover", params)
            assert hover["contents"]["value"] == "int32_t", hover
            definitions = request("textDocument/definition", params)
            definition = definitions[0] if isinstance(definitions, list) else definitions
            assert definition["uri"] == uri, definitions
            assert definition["range"]["start"] == lsp_position(SOURCE, SOURCE.index("demanded =")), definitions

            # A consuming member of an ordinary source-defined class uses the
            # general consuming-receiver diagnostic, anchored at the member.
            named = SOURCE.replace("std::move(carrier).expect", "carrier.expect")
            errors = change(2, named)
            error = next((item for item in errors if item.get("code") == "GTI-S2022"), None)
            assert error is not None and "std::move" in error["message"], errors
            # The general consuming-receiver diagnostic anchors the call's
            # closing parenthesis.
            close = named.index(")", named.index(".expect"))
            assert error["range"] == {"start": lsp_position(named, close),
                                      "end": lsp_position(named, close + 1)}, error

            conflict = SOURCE.replace("int observed = alias;\n  owner = 8;",
                                      "/*😀*/ owner = 8;\n  int observed = alias;")
            errors = change(3, conflict)
            error = next((item for item in errors if item.get("code") == "GTI-S2017"), None)
            assert error is not None, errors
            assert error.get("relatedInformation"), error
            assert error["range"]["start"]["line"] == lsp_position(conflict, conflict.index("owner = 8"))["line"], error

            # Prefix ? must retain the same owner restriction, not just .expect.
            propagated_conflict = """expected<int&, int> lookup(int& owner) { return owner; }
expected<int, int> read() {
  mut int owner = 7;
  int& alias = ?lookup(owner);
  /*😀*/ owner = 8;
  return alias;
}
"""
            errors = change(4, propagated_conflict)
            assert any(item.get("code") == "GTI-S2017" for item in errors), errors
            assert change(5, SOURCE) == []

            # Broken source must not poison a subsequent valid snapshot.
            incomplete = SOURCE.replace('expect("required")', 'expect(')
            assert change(6, incomplete)
            assert change(7, SOURCE) == []
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            open_document(8)
            assert diagnostics(8) == []
        finally:
            session.close()
    print("Borrowed Expected extraction LSP checks passed")


if __name__ == "__main__":
    main()
