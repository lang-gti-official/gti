#!/usr/bin/env python3
"""Executable-only entry diagnostics, direct/project builds and LSP snapshots."""

import pathlib
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession
from lsp_project_facts_test import diagnostics_for, initialize, open_document


CODE = "GTI-S2094"
SOURCE = (
             'export module helper;\n'
             '// supplementary: 😀\n'
             'namespace tea { /* 😀 */ export int main() { return 0; } }\n'
         )


def main():
    compiler, lsp = map(lambda arg: str(pathlib.Path(arg).resolve()), sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="gti-entry-") as directory:
        root = pathlib.Path(directory)
        project = root / "project"
        source_dir = project / "src"
        source_dir.mkdir(parents=True)
        entry = source_dir / "main.gti"
        helper = source_dir / "helper.gti"
        standalone = root / "standalone.gti"
        helper.write_text(SOURCE)
        standalone.write_text(SOURCE)
        entry.write_text(SOURCE)
        (project / "gti.toml").write_text(
            'manifest-version = 1\n'
            '[package]\nname = "entry-test"\nversion = "0.1.0"\n'
            '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n'
            '[targets.checks]\nkind = "test"\nroot = "src/checks.gti"\n'
        )
        test_root = source_dir / "checks.gti"
        test_root.write_text(SOURCE)
        library_dir = root / "library"
        library_dir.mkdir()
        (library_dir / "gti.toml").write_text(
            'manifest-version = 1\n'
            '[package]\nname = "library"\nversion = "0.1.0"\nsource-root = "."\n')
        library = library_dir / "library.gti"
        library.write_text(SOURCE)

        def run(*args, cwd=root, error=False):
            result = subprocess.run([compiler, *map(str, args)], cwd=cwd,
                                    text=True, capture_output=True, timeout=90)
            output = result.stdout + result.stderr
            assert (result.returncode != 0) == error, output
            return output

        # No compiler invocation is needed to reject the source. Emit-only is
        # still valid for library code and does not invent executable intent.
        output = run(entry, "--cxx", root / "no-native-compiler", error=True)
        assert CODE in output and "global 'main'" in output, output
        assert "tea::main" in output and "native C++ compiler" not in output, output
        run(standalone, "--emit-cpp", "-o", root / "library.cpp")
        run(standalone, "--emit-mir", "-o", root / "library.mir")
        assert CODE in run("check", cwd=project, error=True)
        assert CODE in run("check", "checks", cwd=project, error=True)
        assert CODE in run("build", "--no-cache", cwd=project, error=True)
        run("build", "--emit-mir", cwd=project)

        entry.write_text("int helper() { return 0; }\n")
        assert CODE in run("check", cwd=project, error=True)
        entry.write_text('namespace tea { int main() { return 0; } }\n'
                         'int main() { return tea::main(); }\n')
        run("check", cwd=project)
        run("build", "--no-cache", cwd=project)
        run("build", cwd=project)
        run("build", cwd=project)
        entry.write_text(SOURCE)
        assert CODE in run("build", cwd=project, error=True)
        # An included ordinary function named main is not an entry candidate.
        entry.write_text('import "helper.gti";\nint main() { return tea::main(); }\n')
        run("check", cwd=project)
        entry.write_text('import "helper.gti";\n')
        assert CODE in run("check", cwd=project, error=True)
        entry.write_text(SOURCE)

        session = LspSession(lsp)
        try:
            initialize(session)
            uri = open_document(session, entry, SOURCE)
            errors = diagnostics_for(session, uri, 1)
            diagnostic = next(item for item in errors if item.get("code") == CODE)
            assert diagnostic["range"] == {
                "start": {"line": 1, "character": 29},
                "end": {"line": 1, "character": 33}}, diagnostic
            assert "global 'main'" in diagnostic["message"], diagnostic

            helper_uri = open_document(session, helper, SOURCE)
            assert not diagnostics_for(session, helper_uri, 1)
            standalone_uri = open_document(session, standalone, SOURCE)
            assert not diagnostics_for(session, standalone_uri, 1)
            library_uri = open_document(session, library, SOURCE)
            assert not diagnostics_for(session, library_uri, 1)
            test_uri = open_document(session, test_root, SOURCE)
            assert any(item.get("code") == CODE for item in diagnostics_for(session, test_uri, 1))

            def change(version, text):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                              "params": {"textDocument": {"uri": uri, "version": version},
                                         "contentChanges": [{"text": text}]}})
                return diagnostics_for(session, uri, version)

            fixed = SOURCE + "int main() { return tea::main(); }\n"
            assert not change(2, fixed)
            assert not any(item.get("code") == CODE for item in
                           change(3, "namespace tea { int main( {"))
            assert any(item.get("code") == CODE for item in change(4, SOURCE))
            assert not change(5, fixed)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            diagnostics_for(session, uri)
            open_document(session, entry, SOURCE, version=6)
            assert any(item.get("code") == CODE for item in diagnostics_for(session, uri, 6))
        finally:
            session.close()
    print("executable entry CLI/LSP checks passed")


if __name__ == "__main__":
    main()
