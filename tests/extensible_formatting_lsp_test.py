#!/usr/bin/env python3
"""Formatter contracts use compiler diagnostics and current LSP snapshots."""
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, has_error_diagnostics, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-format-lsp-") as temporary:
        source = ('import std.format;\nimport std.string;\n'
                  'struct AssetId { AssetId() = default; uint64_t value = uint64_t(42); };\n'
                  'int main() { AssetId id{}; std::println("{}", /* 😀 */ id); return 0; }\n')
        path = pathlib.Path(temporary) / "formatter.gti"
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

            rejected = diagnostics(1)
            diagnostic = next(item for item in rejected["diagnostics"] if item.get("code") == "GTI-S2029")
            assert "formattable<AssetId>" in diagnostic["message"], diagnostic
            offset = source.index("id);")
            assert diagnostic["range"] == {"start": lsp_position(source, offset),
                                            "end": lsp_position(source, offset + 2)}, diagnostic
            assert diagnostic["data"]["phase"] == "semantics" and not diagnostic["data"].get("fixes"), diagnostic
            assert any("format" in note["message"] for note in diagnostic["relatedInformation"]), diagnostic
            specialization = '''class std::formatter<AssetId> {
public:
  formatter() = default;
  expected<void, std::format_errc> format(AssetId& value, mut std::string& output, std::format_spec& spec) {
    std::formatter<uint64_t> numeric{};
    return numeric.format(value.value, output, spec);
  }
};
'''
            repaired = source.replace("int main()", specialization + "int main()")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": repaired}]}})
            assert not has_error_diagnostics(diagnostics(2))
            wrong = repaired.replace("mut std::string& output", "std::string& output")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 3},
                                     "contentChanges": [{"text": wrong}]}})
            assert any(item.get("code") == "GTI-S2029" for item in diagnostics(3)["diagnostics"])
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 4},
                                     "contentChanges": [{"text": repaired}]}})
            assert not has_error_diagnostics(diagnostics(4))
        finally:
            session.close()
    print("formatter LSP constraints, UTF-16 source blame and overlay repair passed")


if __name__ == "__main__":
    main()
