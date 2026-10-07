#!/usr/bin/env python3
"""Recursive pointer qualifiers and native calls use compiler-owned LSP facts."""

import os
from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    root = Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(root / "stdlib")
    source = """using Slot = c_int* const*;
using Writer = (c_int**, c_int*) -> void;
extern "C" { Writer obtain_writer(); }
int main() {
  mut c_int value = 42;
  mut c_int* pointer = nullptr;
  Slot readonly = &pointer;
  unsafe {
    Writer callback = obtain_writer();
    /*😀*/ callback(&pointer, &value);
    return **readonly - 42;
  }
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-nested-pointer-lsp-") as directory:
        path = Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 10

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
            return [item for item in reply["params"]["diagnostics"]
                    if item.get("severity") == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def at(text, offset):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, offset)}

        try:
            request("initialize", {"capabilities": {}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": source}}})
            assert not diagnostics(1)
            hover = request("textDocument/hover", at(source, source.index("readonly =") + 2))
            assert "int32_t* const*" in str(hover), hover
            call = source.index("callback(&")
            hover = request("textDocument/hover", at(source, call + 2))
            assert "int32_t**" in str(hover) and "void" in str(hover), hover
            definition = request("textDocument/definition", at(source, call + 2))
            assert definition and str(uri) in str(definition), definition
            invalid = source.replace("return **readonly - 42;", "*readonly = nullptr; return 0;")
            change(2, invalid)
            errors = diagnostics(2)
            error = next((item for item in errors if item.get("code") == "GTI-S2002"
                          and "const raw pointer memory" in item["message"]), None)
            assert error, errors
            assert error["range"]["start"] == lsp_position(invalid, invalid.index("*readonly =")), error
            change(3, source)
            assert not diagnostics(3)
            change(4, source.replace("c_int* const*", "c_int* const"))
            assert diagnostics(4)
            change(5, source)
            assert not diagnostics(5)
            formatted = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            assert len(formatted) == 1, formatted
            text = formatted[0]["newText"]
            assert "usingSlot=c_int*const*;" in "".join(text.split()), formatted
            change(6, text)
            assert not diagnostics(6)
            second = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            assert not second or second[0]["newText"] == text, second
        finally:
            session.close()
    print("nested pointer LSP qualifiers, native calls and recovery passed")


if __name__ == "__main__":
    main()
