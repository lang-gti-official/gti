#!/usr/bin/env python3
"""Saved module membership changes refresh editor snapshots without a build."""
import os
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, analysis_kinds, lsp_position


def saved_membership_without_watcher(executable):
    """A normal save must publish new/renamed module identities itself."""
    with tempfile.TemporaryDirectory(prefix="gti-module-save-lsp-") as directory:
        root = pathlib.Path(directory)
        entry, api = root / "src/main.gti", root / "src/api.gti"
        source = "import demo.fresh;\nint main() { return answer(); }\n"
        interface = "export module demo.fresh;\nexport int answer() { return 42; }\n"
        write(entry, source)
        write(root / "gti.toml", 'manifest-version = 1\n[package]\nname = "demo"\n'
              'version = "0.1.0"\n[targets.demo]\nkind = "executable"\nroot = "src/main.gti"\n')
        trace = root / "frontend.jsonl"
        session = LspSession(executable, environment=dict(
            os.environ, GTI_LSP_FRONTEND_TRACE=str(trace)))

        def change(uri, version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def save():
            session.send({"jsonrpc": "2.0", "method": "textDocument/didSave",
                          "params": {"textDocument": {"uri": api_uri}}})

        def definition(request_id, text):
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": "textDocument/definition",
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, text.index("answer") + 1)}})
            return session.receive_until(lambda item: item.get("id") == request_id)

        try:
            initialize(session)
            uri = open_document(session, entry, source)
            assert diagnostics_for(session, uri, 1)
            # Opening an unsaved file must settle before it appears on disk.
            api_uri = open_document(session, api, interface)
            diagnostics_for(session, api_uri, 1)
            write(api, interface)
            save()
            result = definition(31, source)
            assert (result.get("result") or {}).get("uri") == api_uri, result

            renamed_source = source.replace("demo.fresh", "demo.renamed")
            renamed_interface = interface.replace("demo.fresh", "demo.renamed")
            change(uri, 2, renamed_source)
            assert diagnostics_for(session, uri, 2)
            change(api_uri, 2, renamed_interface)
            diagnostics_for(session, api_uri, 2)
            write(api, renamed_interface)
            save()
            result = definition(32, renamed_source)
            assert (result.get("result") or {}).get("uri") == api_uri, result

            # Removing the declaration also removes its named-import identity.
            ordinary = "int answer() { return 42; }\n"
            change(api_uri, 3, ordinary)
            diagnostics_for(session, api_uri, 3)
            write(api, ordinary)
            save()
            assert definition(33, renamed_source).get("result") is None
            change(api_uri, 4, renamed_interface)
            diagnostics_for(session, api_uri, 4)
            write(api, renamed_interface)
            save()
            result = definition(34, renamed_source)
            assert (result.get("result") or {}).get("uri") == api_uri, result

            body_edit = renamed_interface.replace("42", "43")
            change(api_uri, 5, body_edit)
            diagnostics_for(session, api_uri, 5)
            assert (definition(35, renamed_source).get("result") or {}).get("uri") == api_uri
            before_save = analysis_kinds(trace)
            write(api, body_edit)
            save()
            assert (definition(36, renamed_source).get("result") or {}).get("uri") == api_uri
            assert analysis_kinds(trace) == before_save, "body-only saves must not repeat analysis"
            assert not (root / "build").exists()
            assert not (root / "gti.lock").exists()
        finally:
            session.close()


def main():
    saved_membership_without_watcher(sys.argv[1])
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

            duplicate = root / "src/duplicate.gti"
            write(duplicate, interface)
            watched((duplicate, 1))
            errors = diagnostics_for(session, duplicate.resolve().as_uri())
            assert any(item.get("code") == "GTI-B1005" for item in errors), errors
            duplicate.unlink()
            watched((duplicate, 3))
            # Other diagnostics from the duplicate generation may already be
            # in the pipe; wait for the recovered snapshot of this version.
            assert diagnostics_for(session, uri, 1, predicate=lambda items: not items) == []

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
