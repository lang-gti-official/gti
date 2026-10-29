#!/usr/bin/env python3
"""Module queries retain exact UTF-16 ranges and recover across missing imports."""

import json
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-module-navigation-") as temporary:
        root = pathlib.Path(temporary)
        app, library = root / "app", root / "library"
        entry, api = app / "main.gti", library / "api.gti"
        interface = "/* 😀 */ export module net /* trivia */ . http;\nexport int answer() { return 42; }\n"
        source = "/* 😀 */ import dep.net.http;\nimport dep.missing;\nint main() { return answer(); }\n"
        write(entry, source)
        write(api, interface)
        write(app / "gti.toml", 'manifest-version = 1\n[package]\nname = "app"\n'
              'version = "0.1.0"\nsource-root = "."\n[dependencies]\n'
              'dep = { path = "../library" }\n[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        manifest = library / "gti.toml"
        write(manifest, 'manifest-version = 1\n[package]\nname = "library"\n'
              'version = "0.1.0"\nsource-root = "."\n[modules]\n"net.http" = "api.gti"\n')
        session = LspSession(sys.argv[1])
        request_id = 40

        def query(method, text, offset):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, offset)}})
            reply = session.receive_until(lambda value: value.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})
            return diagnostics_for(session, uri, version)

        try:
            initialize(session, watched_files=True)
            uri = open_document(session, entry, source)
            errors = diagnostics_for(session, uri, 1)
            assert any(item.get("code") == "GTI-I0012" for item in errors), errors
            definition = query("textDocument/definition", source, source.index("net.http") + 2)
            assert definition["uri"] == api.resolve().as_uri(), definition
            assert definition["range"] == {
                "start": lsp_position(interface, interface.index("net")),
                "end": lsp_position(interface, interface.index("http") + 4)}, definition
            assert "net.http" in json.dumps(query("textDocument/hover", source, source.index("dep.net")))
            # A loader failure must retain independently available semantic queries.
            answer = query("textDocument/definition", source, source.index("answer") + 1)
            assert answer["uri"] == api.resolve().as_uri(), answer
            assert not query("textDocument/definition", source, source.index("dep.missing") + 5)

            incomplete = "/* 😀 */ import dep /* trivia */ . ne"
            assert change(2, incomplete)
            completion = query("textDocument/completion", incomplete, len(incomplete))
            assert "dep.net.http" in {item["label"] for item in completion["items"]}, completion

            replacement = library / "next.gti"
            write(replacement, "export module net.next;\nexport int next() { return 0; }\n")
            write(manifest, manifest.read_text().replace('"net.http" = "api.gti"',
                                                         '"net.next" = "next.gti"'))
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": manifest.resolve().as_uri(), "type": 2}]}})
            diagnostics_for(session, uri, 2)
            completion = query("textDocument/completion", incomplete, len(incomplete))
            labels = {item["label"] for item in completion["items"]}
            assert "dep.net.next" in labels and "dep.net.http" not in labels, labels
            valid = "import dep.net.next;\nint main() { return next(); }\n"
            assert change(3, valid) == []
            restored = query("textDocument/definition", valid, valid.index("dep.net"))
            assert restored["uri"] == replacement.resolve().as_uri(), restored
            assert not (app / "build").exists()
            assert not (app / "gti.lock").exists()
        finally:
            session.close()


if __name__ == "__main__":
    main()
