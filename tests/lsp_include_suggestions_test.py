#!/usr/bin/env python3
"""General project include discovery uses current frontend fixes, not name lists."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, has_error_diagnostics, lsp_position


def run(executable, root):
    (root / "gti.toml").write_text('''manifest-version = 1
[package]
name = "include-test"
version = "0.1.0"
[targets.app]
kind = "executable"
root = "src/main.gti"
''')
    (root / "src").mkdir()
    (root / "include").mkdir()
    entry = root / "src/main.gti"
    header = root / "include/api.gti"
    source = '// 😀\nint main() { return example::test(); }\n'
    entry.write_text(source)
    header.write_text((
                          'export module api;\n'
                          'namespace example { export int test() { return 7; } }\n'
                      ))
    uri = entry.as_uri()
    header_uri = header.as_uri()
    session = LspSession(executable)
    request_id = 0

    def request(method, params):
        nonlocal request_id
        request_id += 1
        session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        return session.receive_until(lambda msg: msg.get("id") == request_id)["result"]

    def notify(method, params):
        session.send({"jsonrpc": "2.0", "method": method, "params": params})

    def open_file(file_uri, text):
        notify("textDocument/didOpen", {"textDocument": {
            "uri": file_uri, "languageId": "gti", "version": 1, "text": text}})

    def change(file_uri, version, text):
        notify("textDocument/didChange", {"textDocument": {"uri": file_uri, "version": version},
                                         "contentChanges": [{"text": text}]})

    def published(predicate):
        return session.receive_until(lambda msg:
            msg.get("method") == "textDocument/publishDiagnostics"
            and msg["params"].get("uri") == uri
            and predicate(msg["params"]), timeout=20)["params"]

    def include_diagnostic(publication):
        return next((d for d in publication["diagnostics"]
                     if d.get("data", {}).get("fixes")), None)

    def actions(diagnostic):
        return request("textDocument/codeAction", {
            "textDocument": {"uri": uri}, "range": diagnostic["range"],
            "context": {"diagnostics": [diagnostic], "only": ["quickfix"]}})

    try:
        request("initialize", {"capabilities": {
            "workspace": {"workspaceEdit": {"documentChanges": True}},
            "textDocument": {
                "publishDiagnostics": {"dataSupport": True},
                "codeAction": {"isPreferredSupport": True,
                               "codeActionLiteralSupport": {"codeActionKind": {"valueSet": ["quickfix"]}}}}}})
        notify("initialized", {})
        open_file(uri, source)
        first = published(lambda p: include_diagnostic(p) is not None)
        diagnostic = include_diagnostic(first)
        assert diagnostic["range"]["start"] == lsp_position(source, source.index("test")), diagnostic
        assert "(fix available)" in diagnostic["message"], diagnostic
        offered = actions(diagnostic)
        assert len(offered) == 1 and offered[0]["title"] == 'Add import "../include/api.gti";', offered
        edit = offered[0]["edit"]["documentChanges"][0]
        assert edit["textDocument"] == {"uri": uri, "version": 1}, edit
        assert edit["edits"][0]["newText"] == 'import "../include/api.gti";\n', edit

        # An unsaved dependency edit removes the definition; no disk fallback.
        open_file(header_uri, 'namespace example { int renamed() { return 7; } }\n')
        published(lambda p: has_error_diagnostics(p) and include_diagnostic(p) is None)
        assert actions(diagnostic) == [], "stale diagnostic must not offer its old include"
        change(header_uri, 2, 'namespace example { int test() { return 8; } }\n')
        published(lambda p: include_diagnostic(p) is not None)
        notify("textDocument/didClose", {"textDocument": {"uri": header_uri}})
        published(lambda p: include_diagnostic(p) is not None)

        # Deletion and creation must refresh discovery, not just loaded graph edges.
        header.unlink()
        notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": header_uri, "type": 3}]})
        published(lambda p: has_error_diagnostics(p) and include_diagnostic(p) is None)
        other = root / "include/new.gti"
        other.write_text('namespace example { int test() { return 9; } }\n')
        notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": other.as_uri(), "type": 1}]})
        new = include_diagnostic(published(lambda p: include_diagnostic(p) is not None))
        assert 'new.gti' in actions(new)[0]["title"]

        # Applying the offered edit clears the lookup error on the current version.
        replacement = new["data"]["fixes"][0]["replacement"]
        change(uri, 2, replacement + source)
        published(lambda p: p.get("version") == 2 and not has_error_diagnostics(p))
        assert actions(new) == [], "already-applied include must not be offered again"

        # Functions in standard-library files use exactly the same mechanism.
        change(uri, 3, 'int main() { return std::gcd(12, 8); }\n')
        std = include_diagnostic(published(lambda p: p.get("version") == 3 and include_diagnostic(p)))
        assert actions(std)[0]["title"] == 'Add import std.numeric;', std
    finally:
        session.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gti-lsp-includes-") as directory:
        run(str(pathlib.Path(sys.argv[1]).resolve()), pathlib.Path(directory).resolve())
    print("General project include actions passed")
