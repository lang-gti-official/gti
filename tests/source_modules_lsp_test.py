#!/usr/bin/env python3
"""Module exports are the same API in compilation and live editor snapshots."""

import json
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-modules-lsp-") as temporary:
        root = pathlib.Path(temporary)
        app, library = root / "app", root / "library"
        main_path, api_path = app / "src/main.gti", library / "api.gti"
        source = "import lib;\nint main() { return api::answer(); }\n"
        interface = (
            "export module main;\n"
            "namespace api {\n"
            "  int hidden() { return 42; }\n"
            "  export int answer() { return hidden(); }\n"
            "  struct Detail { int read() { return 7; } };\n"
            "  export Detail make() { return Detail{}; }\n"
            "}\n"
        )
        write(main_path, source)
        write(api_path, interface)
        write(app / "gti.toml", 'manifest-version = 1\n[package]\nname = "app"\n'
              'version = "0.1.0"\n[dependencies]\nlib = { path = "../library" }\n'
              '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n')
        write(library / "gti.toml", 'manifest-version = 1\n[package]\nname = "library"\n'
              'version = "0.1.0"\nsource-root = "."\n[modules]\nmain = "api.gti"\n')
        session = LspSession(sys.argv[1])
        request_id = 20

        def request(method, uri, text, offset):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, offset)}})
            response = session.receive_until(lambda value: value.get("id") == request_id)
            assert "error" not in response, response
            return response["result"]

        def completion(uri, text, marker):
            result = request("textDocument/completion", uri, text, text.index(marker) + len(marker))
            return {item["label"] for item in result["items"]}

        def change(uri, version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})

        try:
            initialize(session, watched_files=True)
            uri = open_document(session, main_path, source)
            assert diagnostics_for(session, uri, 1) == []
            labels = completion(uri, source, "api::")
            assert "answer" in labels and "hidden" not in labels and "Detail" not in labels, labels
            location = request("textDocument/definition", uri, source, source.index("answer") + 1)
            assert location["uri"] == api_path.resolve().as_uri(), location
            hover = request("textDocument/hover", uri, source, source.index("answer") + 1)
            assert "answer" in json.dumps(hover), hover

            # A missing return and unfinished expression must not remove imports.
            incomplete = "import lib;\nint broken() { api:: }\nint main() {}\n"
            change(uri, 2, incomplete)
            assert diagnostics_for(session, uri, 2)
            labels = completion(uri, incomplete, "api::")
            assert "answer" in labels and "hidden" not in labels, labels
            change(uri, 3, source)
            assert diagnostics_for(session, uri, 3) == []

            # An unsaved interface edit must update its open consumer's API.
            edited = interface.replace("export int answer", "int answer").replace(
                "int hidden", "export int hidden")
            api_uri = open_document(session, api_path, edited)
            assert diagnostics_for(session, api_uri, 1) == []
            errors = diagnostics_for(session, uri, 3)
            assert any("not exported" in item["message"] for item in errors), errors
            labels = completion(uri, source, "api::")
            assert "hidden" in labels and "answer" not in labels, labels

            # Closing the overlay restores the on-disk interface without a build.
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": api_uri}}})
            assert diagnostics_for(session, uri, 3) == []
            labels = completion(uri, source, "api::")
            assert "answer" in labels and "hidden" not in labels, labels
            inferred = "import lib;\nint main() { auto item = api::make(); return item.read(); }\n"
            change(uri, 4, inferred)
            assert diagnostics_for(session, uri, 4) == []
            assert "read" in completion(uri, inferred, "item."), "a reachable value retains public members"
            assert not (app / "build").exists(), "module completion must remain read-only"
            assert not (app / "gti.lock").exists(), "module completion must not fetch dependencies"
        finally:
            session.close()


if __name__ == "__main__":
    main()
