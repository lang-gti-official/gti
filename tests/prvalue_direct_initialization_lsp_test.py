#!/usr/bin/env python3
"""Direct initialization uses compiler-owned facts in current editor snapshots."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, has_error_diagnostics, lsp_position


SOURCE = """class Fixed {
public:
  Fixed() {}
  Fixed(Fixed&) = delete;
  Fixed(Fixed&&) = delete;
};
Fixed make() { return Fixed(); }
int main() {
  Fixed source{};
  /* 😀 */ Fixed result = Fixed(make());
  return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-fresh-result-lsp-") as temporary:
        path = pathlib.Path(temporary) / "main.gti"
        path.write_text(SOURCE, encoding="utf-8")
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def publication(version):
            return session.receive_until(
                lambda message: message.get("method") == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == uri
                and message["params"].get("version") == version)["params"]

        try:
            request("initialize", {"capabilities": {}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": SOURCE}}})
            assert not has_error_diagnostics(publication(1))
            offset = SOURCE.index("Fixed(make())")
            params = {"textDocument": {"uri": uri},
                      "position": lsp_position(SOURCE, offset)}
            hover = request("textDocument/hover", params)
            assert hover and "no additional copy, move" in hover["contents"]["value"], hover
            assert hover["range"]["start"] == lsp_position(SOURCE, offset), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location and location["uri"] == uri, definition
            invalid = SOURCE.replace("Fixed(make())", "Fixed(std::move(source))")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": invalid}]}})
            assert has_error_diagnostics(publication(2))
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 3},
                                     "contentChanges": [{"text": SOURCE}]}})
            assert not has_error_diagnostics(publication(3))
            assert request("textDocument/hover", params) == hover
        finally:
            session.close()


if __name__ == "__main__":
    main()
