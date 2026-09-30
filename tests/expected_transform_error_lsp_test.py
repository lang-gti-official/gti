#!/usr/bin/env python3
"""Expected error mapping shares compiler diagnostics and member completion."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = """enum class Error { failed };
Error map(int error) { return Error::failed; }
expected<int, int> read() { return unexpected(1); }
int main() {
  auto source = read();
  /*😀*/ auto mapped = std::move(source).transform_error(map);
  return mapped.has_value() ? 1 : 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-transform-error-lsp-") as directory:
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
            reply = session.receive_until(lambda message: message.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def open_document(version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": version, "text": source}}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(1)
            assert not diagnostics(1)

            # The mapper keeps its ordinary declaration identity for navigation.
            mapper_offset = source.rindex("map)")
            mapper_position = {"textDocument": {"uri": uri},
                               "position": lsp_position(source, mapper_offset)}
            hover = request("textDocument/hover", mapper_position)
            assert "map" in str(hover) and "Error" in str(hover), hover
            definitions = request("textDocument/definition", mapper_position)
            definition = definitions[0] if isinstance(definitions, list) else definitions
            assert definition["uri"] == uri, definitions
            assert definition["range"]["start"] == lsp_position(source, source.index("map(")), definitions

            wrong_mapper = source.replace("Error map(int error)", "Error map(bool error)")
            change(2, wrong_mapper)
            errors = diagnostics(2)
            error = next((d for d in errors if d.get("code") == "GTI-S2097"), None)
            assert error is not None, errors
            start = wrong_mapper.rindex("map)")
            assert error["range"] == {"start": lsp_position(wrong_mapper, start),
                                      "end": lsp_position(wrong_mapper, start + 3)}, error
            assert error.get("relatedInformation"), error
            assert error.get("data", {}).get("hints"), error
            assert "non-generic namespace function" in error["message"], error

            named_receiver = source.replace("std::move(source)", "source")
            change(3, named_receiver)
            errors = diagnostics(3)
            error = next((d for d in errors if d.get("code") == "GTI-S2018"), None)
            assert error is not None, errors
            start = named_receiver.index("transform_error")
            assert error["range"] == {"start": lsp_position(named_receiver, start),
                                      "end": lsp_position(named_receiver, start + 15)}, error
            assert "std::move" in error["message"], error

            incomplete = source.replace("transform_error(map)", "")
            change(4, incomplete)
            assert diagnostics(4)
            offset = incomplete.index("std::move(source).") + len("std::move(source).")
            completions = request("textDocument/completion", {
                "textDocument": {"uri": uri}, "position": lsp_position(incomplete, offset)})
            items = completions["items"] if isinstance(completions, dict) else completions
            transform = next((item for item in items if item["label"] == "transform_error"), None)
            assert transform is not None, items
            assert "&&" in transform.get("detail", ""), transform

            change(5, source)
            assert not diagnostics(5)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            open_document(6)
            assert not diagnostics(6)
        finally:
            session.close()


if __name__ == "__main__":
    main()
