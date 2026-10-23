#!/usr/bin/env python3
"""Test-target entry semantics and manifest invalidation through LSP."""

import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession


def manifest(kind="test"):
    return (
        'manifest-version = 1\n[package]\nname = "suite"\nversion = "0.1.0"\n'
        f'[targets.suite]\nkind = "{kind}"\nroot = "src/suite.gti"\n'
    )


def errors(items):
    return [item for item in items if item.get("severity", 1) == 1]


def test_contracts(executable, root):
    cases = [
        ("namespaced", 'namespace checks { [[test]] void addition() {} }\n', None),
        ("testing-header", '#include <std/testing>\nnamespace checks {\n'
         '[[test]] void addition() { std::testing::check(1 + 2 == 3); }\n}\n', None),
        ("legacy", 'int main() { return 0; }\n', None),
        ("ambiguous", '[[test]] void addition() {}\n', None),
        ("empty", 'int helper() { return 1; }\n', "GTI-S2094"),
        ("invalid-signature", '[[test]] int invalid() { return 0; }\n', "GTI-S2099"),
        ("conflicting-main", '[[test]] void addition() {}\nint main() { return 0; }\n', "GTI-S2099"),
    ]
    session = LspSession(executable)
    try:
        initialize(session)
        for name, source, expected in cases:
            project = root / name
            config = manifest()
            if name == "ambiguous":
                config += '[targets.app]\nkind = "executable"\nroot = "src/suite.gti"\n'
            write(project / "gti.toml", config)
            path = project / "src/suite.gti"
            write(path, source)
            uri = open_document(session, path, source)
            items = diagnostics_for(session, uri, version=1)
            if expected is None:
                assert errors(items) == [], (name, items)
            else:
                assert any(item.get("code") == expected for item in errors(items)), (name, items)
                if name == "empty":
                    entry = next(item for item in items if item.get("code") == expected)
                    assert "Test target requires" in entry["message"], items
                    assert entry["range"]["start"] == {"line": 0, "character": 0}, entry
    finally:
        session.close()


def test_manifest_switch(executable, root):
    project = root / "switch"
    config = project / "gti.toml"
    source = 'namespace checks { [[test]] void addition() {} }\n'
    path = project / "src/suite.gti"
    write(path, source)
    write(config, manifest("executable"))
    session = LspSession(executable)
    try:
        initialize(session, watched_files=True)
        uri = open_document(session, path, source)
        items = diagnostics_for(session, uri, version=1)
        assert any(item.get("code") == "GTI-S2094" for item in items), items
        for kind in ("test", "executable", "test"):
            write(config, manifest(kind))
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": config.resolve().as_uri(), "type": 2}]}})
            items = diagnostics_for(session, uri, version=1)
            if kind == "test":
                assert errors(items) == [], items
            else:
                entry = next((item for item in items if item.get("code") == "GTI-S2094"), None)
                assert entry is not None and "Executable requires" in entry["message"], items
    finally:
        session.close()


def test_fragments_and_edits(executable, root):
    project = root / "edits"
    write(project / "gti.toml", manifest())
    path = project / "src/suite.gti"
    source = 'namespace checks { [[test]] void addition() {} }\n'
    write(path, source)
    fragment = project / "src/helper.gti"
    helper = 'int helper() { return 1; }\n'
    write(fragment, helper)
    session = LspSession(executable)
    try:
        initialize(session)
        uri = open_document(session, path, source)
        assert diagnostics_for(session, uri, version=1) == []
        helper_uri = open_document(session, fragment, helper)
        assert diagnostics_for(session, helper_uri, version=1) == []
        # Ordinary source diagnostics must survive test-entry selection and
        # stay attached to the current, unexpanded document text/version.
        warning = 'namespace checks { [[test]] void addition() { mut int count = 1; } }\n'
        for version, text in ((2, warning), (3, source)):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})
            items = diagnostics_for(session, uri, version=version)
            assert errors(items) == [], items
            if version == 2:
                assert any(item.get("code") == "GTI-S2096" for item in items), items
            else:
                assert items == [], items
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol",
                      "params": {"textDocument": {"uri": uri}}})
        symbols = session.receive_until(lambda message: message.get("id") == 2)["result"]
        def names(nodes):
            return [node["name"] for node in nodes] + [
                name for node in nodes for name in names(node.get("children", []))]
        symbol_names = names(symbols)
        assert "addition" in symbol_names, symbols
        assert "main" not in symbol_names, symbols
    finally:
        session.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_test_target_test.py /path/to/gti_lsp")
    with tempfile.TemporaryDirectory(prefix="gti-lsp-test-target-") as temporary:
        root = pathlib.Path(temporary)
        test_contracts(sys.argv[1], root)
        test_manifest_switch(sys.argv[1], root)
        test_fragments_and_edits(sys.argv[1], root)


if __name__ == "__main__":
    main()
