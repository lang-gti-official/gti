#!/usr/bin/env python3
"""Enum defaults, exact overrides and extraction share compiler LSP facts."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import LspSession, has_error_diagnostics, lsp_position


def main():
    source = ('#include <std/format>\n#include <std/string>\n'
              'enum class Status : uint64_t { failed = 18446744073709551615 };\n'
              'int main() { Status value = Status::failed; '
              'uint64_t raw = std::to_underlying<uint64_t, Status>(value); '
              'std::println("{}", /* 😀 */ value); return 0; }\n')
    with tempfile.TemporaryDirectory(prefix="gti-enum-lsp-") as temporary:
        path = pathlib.Path(temporary) / "enum.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                       "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda item:
                    item.get("method") == "textDocument/publishDiagnostics" and
                    item["params"]["uri"] == uri and item["params"].get("version") == version)["params"]

            assert not has_error_diagnostics(diagnostics(1))
            broken = source.replace("int main()", "class std::formatter<Status> {};\nint main()")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": broken}]}})
            rejected = diagnostics(2)
            diagnostic = next(item for item in rejected["diagnostics"] if item.get("code") == "GTI-S2029")
            assert "formattable<Status>" in diagnostic["message"], diagnostic
            offset = broken.index("value); return")
            assert diagnostic["range"] == {"start": lsp_position(broken, offset),
                                            "end": lsp_position(broken, offset + 5)}, diagnostic
            assert diagnostic["data"]["phase"] == "semantics", diagnostic
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 3},
                                     "contentChanges": [{"text": source}]}})
            assert not has_error_diagnostics(diagnostics(3))
        finally:
            session.close()
    print("integral enum LSP defaults, malformed override, UTF-16 blame and repair passed")


if __name__ == "__main__":
    main()
