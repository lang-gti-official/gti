#!/usr/bin/env python3
"""Pointer alias conversion uses compiler diagnostics and alias identities."""
import os
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position
from lsp_project_facts_test import (
    AUDIT_TEST_ENVIRONMENT,
    audited_diagnostics,
    diagnostics_for,
    open_document,
)


def source_result_policy(lsp, path):
    source = """using Address = void*;
using Bytes = uint8_t*;
using Pointer = int*;
Address byte_transport(Address address) {
  Bytes bytes = Bytes(address);
  return Address(bytes);
}
Target restore<Target>(Address address) { return Target(address); }
int main() {
  Address address = byte_transport(nullptr);
  /*😀*/ Pointer result = restore<Pointer>(address);
  return result == nullptr ? 0 : 1;
}
"""
    path.write_text(source)
    for mode, severity in (("off", None), ("warn", 2), ("strict", 1)):
        session = LspSession(lsp, [f"--raw-pointer-audit={mode}"],
                             environment=AUDIT_TEST_ENVIRONMENT)
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}}})
            assert "error" not in session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            uri = open_document(session, path, source)
            diagnostics = audited_diagnostics(session, uri, 1, mode != "off")
            findings = [d for d in diagnostics
                        if d.get("code") == "GTI-S2102"
                        and "Raw pointer result may contradict" in d["message"]]
            if severity is None:
                assert not findings, diagnostics
            else:
                assert findings and all(d["severity"] == severity for d in findings), diagnostics
                closing = source.index("(address)", source.index("restore<Pointer>")) + len("(address")
                assert findings[0]["range"]["start"] == lsp_position(source, closing), findings
                assert findings[0].get("relatedInformation"), findings
            if mode != "strict":
                assert not [d for d in diagnostics if d.get("severity") == 1], diagnostics
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": "int main() { return 0; }\n"}]}})
            repaired = audited_diagnostics(session, uri, 2, mode != "off")
            assert not [d for d in repaired if d.get("code", "").startswith("GTI-S210")], repaired
        finally:
            session.close()

def main():
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    valid = """using Erased = const void*;
using Pointer = const int*;
int main() { int value = 42; Erased address = Erased(&value);
/*😀*/ Pointer restored = Pointer(address);
unsafe { return *restored - 42; } }
"""
    invalid = valid.replace("using Pointer = const int*;", "using Pointer = const uint64_t*;")
    with tempfile.TemporaryDirectory(prefix="gti-pointer-conversion-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(valid)
        uri = path.as_uri()
        session = LspSession(sys.argv[1], ["--raw-pointer-audit=strict"],
                             environment=AUDIT_TEST_ENVIRONMENT)
        def published(version):
            # The edit-path publication, then the background audit's.
            return [d for d in audited_diagnostics(session, uri, version)
                    if d.get("severity") == 1]
        def change(version, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}}})
            assert "error" not in session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": valid}}})
            assert not published(1)
            change(2, invalid)
            errors = published(2)
            diagnostic = next((d for d in errors if d.get("code") == "GTI-S2101"
                               and "known pointee type" in d["message"]), None)
            assert diagnostic, errors
            assert diagnostic["range"]["start"] == lsp_position(invalid, invalid.index("Pointer(address)")), diagnostic
            assert diagnostic.get("relatedInformation"), diagnostic
            change(3, valid)
            assert not published(3)
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, valid.index("Pointer(address)"))}})
            reply = session.receive_until(lambda message: message.get("id") == 2)
            assert reply.get("result"), reply
            change(4, valid.replace("Pointer(address)", "Pointer("))
            assert published(4)
            change(5, valid)
            assert not published(5)
        finally:
            session.close()
        source_result_policy(sys.argv[1], pathlib.Path(directory) / "result.gti")
    print("pointer conversion LSP diagnostics, alias identity and recovery passed")

if __name__ == "__main__":
    main()
