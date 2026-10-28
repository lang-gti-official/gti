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
            # The mapper is an ordinary generic function-item parameter: the
            # general function-item selection diagnostic names the candidate.
            error = next((d for d in errors if d.get("code") == "GTI-S2090"), None)
            assert error is not None, errors
            start = wrong_mapper.rindex("map)")
            assert error["range"] == {"start": lsp_position(wrong_mapper, start),
                                      "end": lsp_position(wrong_mapper, start + 3)}, error
            assert error.get("relatedInformation"), error
            assert error.get("data", {}).get("hints"), error
            assert "non-generic namespace-scope GTI function" in error["message"], error

            named_receiver = source.replace("std::move(source)", "source")
            change(3, named_receiver)
            errors = diagnostics(3)
            error = next((d for d in errors if d.get("code") == "GTI-S2022"), None)
            assert error is not None, errors
            # The general consuming-receiver diagnostic anchors the call's
            # closing parenthesis.
            close = named_receiver.index("transform_error(map)") + len("transform_error(map")
            assert error["range"] == {"start": lsp_position(named_receiver, close),
                                      "end": lsp_position(named_receiver, close + 1)}, error
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
            for name in ("transform", "and_then", "expect"):
                candidate = next((item for item in items if item["label"] == name), None)
                assert candidate is not None, items
                assert "&&" in candidate.get("detail", ""), candidate

            change(5, source)
            assert not diagnostics(5)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            open_document(6)
            assert not diagnostics(6)
            lambda_source = source.replace("transform_error(map)",
                "transform_error([](int error) -> Error { return Error::failed; })")
            change(7, lambda_source)
            assert not diagnostics(7)
            wrong_lambda = lambda_source.replace("[](int error)", "[](bool error)")
            change(8, wrong_lambda)
            errors = diagnostics(8)
            # The mismatch is found inside the library's transform_error
            # instance and reported at the requesting member call.
            error = next((d for d in errors if d.get("code") == "GTI-S2003"), None)
            assert error is not None, errors
            start = wrong_lambda.index("transform_error")
            assert error["range"] == {"start": lsp_position(wrong_lambda, start),
                                      "end": lsp_position(wrong_lambda, start + 15)}, error
            assert "requires 'bool'" in error["message"], error
            assert any("library instance" in r["message"]
                       for r in error.get("relatedInformation", [])), error
            change(9, lambda_source)
            assert not diagnostics(9)

            # The new members use the same snapshot-owned semantic records.
            ergonomic = """expected<int, int> read() { return 7; }
int main() {
  /*😀*/ return read().transform([](int value) -> int { return value + 1; })
      .and_then([](int value) -> expected<int, int> { return value; })
      .expect("required value");
}
"""
            change(10, ergonomic)
            assert not diagnostics(10)
            wrong_error = ergonomic.replace("-> expected<int, int>", "-> expected<int, bool>")
            change(11, wrong_error)
            errors = diagnostics(11)
            # and_then's result contract is the library's expected_result
            # concept; a wrong error type fails that constraint.
            error = next((d for d in errors if d.get("code") == "GTI-S2029"), None)
            assert error is not None, errors
            # The constraint belongs to the and_then call's deduced mapper
            # result; it is reported at that call's closing parenthesis.
            start = wrong_error.index(")", wrong_error.index("return value; }"))
            assert error["range"] == {"start": lsp_position(wrong_error, start),
                                      "end": lsp_position(wrong_error, start + 1)}, error
            assert "expected_result" in error["message"], error
            wrong_message = ergonomic.replace('"required value"', "42")
            change(12, wrong_message)
            errors = diagnostics(12)
            error = next((d for d in errors if d.get("code") == "GTI-S2003"), None)
            assert error is not None, errors
            start = wrong_message.rindex("42")
            assert error["range"] == {"start": lsp_position(wrong_message, start),
                                      "end": lsp_position(wrong_message, start + 2)}, error
            assert "std::string_view" in error["message"], error
            change(13, ergonomic)
            assert not diagnostics(13)
            owning_enum = """struct Resource { Resource() = default; ~Resource() {} };
enum class Packet { item(Resource value), idle };
expected<int, int> read() { return 7; }
Packet map(int input) { return Packet::item(Resource()); }
int main() { /*😀*/ [[discard]] read().transform(map); return 0; }
"""
            # A source-defined mapping transports an owning payload enum like
            # any other class result; the retired builtin slice restriction
            # no longer exists.
            change(14, owning_enum)
            assert not diagnostics(14)
            change(15, ergonomic)
            assert not diagnostics(15)
        finally:
            session.close()


if __name__ == "__main__":
    main()
