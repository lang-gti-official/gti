#!/usr/bin/env python3
"""Saved module membership changes refresh editor snapshots without a build."""
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-module-discovery-lsp-") as directory:
        root = pathlib.Path(directory)
        entry, api, body = (root / "src/main.gti", root / "src/api.gti",
                            root / "src/body.gti")
        source = "import demo.counter;\nint main() { return answer(); }\n"
        interface = "export module demo.counter;\nexport int answer();\n"
        write(entry, source)
        write(root / "gti.toml", 'manifest-version = 1\n[package]\nname = "demo"\n'
              'version = "0.1.0"\n[targets.demo]\nkind = "executable"\nroot = "src/main.gti"\n')
        session = LspSession(sys.argv[1])

        def watched(*changes):
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": p.as_uri(), "type": kind}
                                                  for p, kind in changes]}})

        try:
            initialize(session, watched_files=True)
            uri = open_document(session, entry, source)
            assert diagnostics_for(session, uri, 1), "missing module must be diagnosed"
            write(api, interface)
            write(body, "module demo.counter;\nint answer() { return 42; }\n")
            watched((api, 1), (body, 1))
            assert diagnostics_for(session, uri, 1) == [], "new files must refresh the module map"
            session.send({"jsonrpc": "2.0", "id": 30, "method": "textDocument/definition",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(source, source.index("answer") + 1)}})
            response = session.receive_until(lambda item: item.get("id") == 30)
            assert response["result"]["uri"] == body.resolve().as_uri(), response

            # Existing unsaved API overlays still use frontend dependency generations.
            api_uri = open_document(session, api, interface.replace("export int", "int"))
            errors = diagnostics_for(session, uri, 1)
            assert any("not exported" in item["message"] for item in errors), errors
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": api_uri}}})
            assert diagnostics_for(session, uri, 1) == []

            api.unlink()
            body.unlink()
            watched((api, 3), (body, 3))
            assert diagnostics_for(session, uri, 1), "removed modules cannot remain cached"
            write(api, interface)
            write(body, "module demo.counter;\nint answer() { return 42; }\n")
            watched((api, 1), (body, 1))
            assert diagnostics_for(session, uri, 1) == []
            assert not (root / "build").exists()
            assert not (root / "gti.lock").exists()
        finally:
            session.close()


if __name__ == "__main__":
    main()
