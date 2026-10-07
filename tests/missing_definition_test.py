#!/usr/bin/env python3
"""Closed programs reject missing GTI bodies before native linking."""

import pathlib
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession
from lsp_project_facts_test import diagnostics_for, open_document


def main():
    compiler, lsp = map(lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:3])
    fixtures = pathlib.Path(__file__).parent / "coverage/reproducers"
    with tempfile.TemporaryDirectory(prefix="gti-missing-definition-") as directory:
        root = pathlib.Path(directory)
        for name in ("f7-undefined-user-function", "f7-declaration-only-weak-ptr",
                     "f7-json-decode-without-codec"):
            rejected = subprocess.run(
                [compiler, str(fixtures / f"{name}.gti"), "--cxx",
                 str(root / "no-native-compiler"), "--progress=never",
                 "--color=never", "-o", str(root / name)],
                capture_output=True, text=True, timeout=90)
            text = rejected.stdout + rejected.stderr
            assert rejected.returncode == 65, (name, rejected)
            assert "GTI-S2106" in text and "has no definition" in text, text
            assert "GTI-B0001" not in text and "native C++ compiler" not in text, text
        source = "// \U0001f600\nint helper(int value);\nint main() { return helper(7); }\n"
        path = root / "src/main.gti"
        path.parent.mkdir()
        path.write_text(source)
        (root / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "closure"\nversion = "0.1.0"\n'
            '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n')
        checked = subprocess.run([compiler, "check", "--progress=never", "--color=never"],
                                 cwd=root, capture_output=True, text=True, timeout=90)
        assert checked.returncode == 65 and "GTI-S2106" in checked.stderr, checked
        session = LspSession(lsp)
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            uri = open_document(session, path, source)
            diagnostics = diagnostics_for(session, uri, 1)
            missing = [item for item in diagnostics if item.get("code") == "GTI-S2106"]
            assert len(missing) == 1, diagnostics
            diagnostic = missing[0]
            assert diagnostic["range"] == {
                "start": {"line": 2, "character": 20},
                "end": {"line": 2, "character": 26}}, diagnostic
            related = diagnostic["relatedInformation"]
            assert len(related) == 1 and related[0]["location"]["uri"] == uri, diagnostic
            assert related[0]["location"]["range"] == {
                "start": {"line": 1, "character": 4},
                "end": {"line": 1, "character": 10}}, diagnostic
            fixed = source.replace("int helper(int value);",
                                   "int helper(int value) { return value; }")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": fixed}]}})
            assert not diagnostics_for(session, uri, 2)
        finally:
            session.close()


if __name__ == "__main__":
    main()
