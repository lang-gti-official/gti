#!/usr/bin/env python3
"""C-thread profile/unsafe diagnostics and selected public declarations."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def check(executable, concurrent, selected="thrd_join"):
    call = {"thrd_join": "return std::thrd_join(handle, result);",
            "mtx_lock": "return std::mtx_lock(handle);",
            "cnd_wait": "return std::cnd_wait(handle, handle);",
            "call_once": "std::call_once(flag, initialize); return 0;"}[selected]
    declarations = ("void initialize() {}\n" if selected == "call_once" else "")
    locals = ("std::once_flag flag{}; " if selected == "call_once" else
              "std::thrd_t handle = 0; mut int32_t result = 0; "
              if selected == "thrd_join" else "std::thrd_t handle = 0; ")
    source = ("// 😀 source-position control\nimport std.cthread;\n"
              + declarations + "int main() { " + locals
              + (call if concurrent else "unsafe { " + call + " }") + " }\n")
    with tempfile.TemporaryDirectory(prefix="gti-cthread-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(executable, ["--execution-profile", "concurrent"] if concurrent else [])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            errors = diagnostics(1)
            code = "GTI-S2055" if concurrent else "GTI-S2084"
            closing = source.index(");", source.index(call))
            error = next(d for d in errors if d["code"] == code and
                         d["range"]["start"] == lsp_position(source, closing))
            assert error["range"] == {"start": lsp_position(source, closing),
                                      "end": lsp_position(source, closing + 1)}, error
            valid = source.replace(call, "unsafe { " + call + " }") if concurrent else (
                "import std.cthread;\nint main() { std::thrd_yield(); return 0; }\n")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert (diagnostics(2) == [])
            if concurrent:
                position = lsp_position(valid, valid.index(selected) + 2)
                session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                    "textDocument": {"uri": uri}, "position": position}})
                hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
                assert "unsafe" in hover["contents"]["value"], hover
                assert selected in hover["contents"]["value"], hover
                session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition", "params": {
                    "textDocument": {"uri": uri}, "position": position}})
                definition = session.receive_until(lambda message: message.get("id") == 3)["result"]
                locations = definition if isinstance(definition, list) else [definition]
                assert any(item["uri"].endswith("/stdlib/std/cthread.gti") for item in locations), definition
        finally:
            session.close()


def check_confined(executable):
    source = ("// 😀 callback contract\nimport std.cthread;\n"
              "using Callback = (int32_t) -> int32_t;\n"
              "int32_t transform(int32_t value) { return value; }\n"
              "int main() { Callback callback = transform; return 0; }\n")
    with tempfile.TemporaryDirectory(prefix="gti-confined-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(executable, ["--execution-profile", "concurrent"])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]
            assert any(d["code"] == "GTI-S2076" for d in diagnostics(1))
            valid = source.replace("int32_t transform", "[[thread_confined_callback]] int32_t transform")
            for version, text in ((2, valid), (3, valid.replace("] int32_t transform", "] unsafe int32_t transform")), (4, valid)):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})
                errors = diagnostics(version)
                if version == 3:
                    assert any(d["code"] == "GTI-S2099" for d in errors), errors
                else:
                    assert (errors == []), errors
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, valid.index("transform") + 2)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "[[thread_confined_callback]]" in hover["contents"]["value"], hover
        finally:
            session.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cthread_lsp_test.py <gti_lsp>")
    check(sys.argv[1], False)
    check(sys.argv[1], True)
    check_confined(sys.argv[1])
    for selected in ("mtx_lock", "cnd_wait", "call_once"):
        check(sys.argv[1], False, selected)
        check(sys.argv[1], True, selected)


if __name__ == "__main__":
    main()
