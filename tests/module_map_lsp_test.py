#!/usr/bin/env python3
"""An installed public-map edit invalidates existing editor snapshots."""

import json
import os
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-module-map-lsp-") as temporary:
        root = pathlib.Path(temporary)
        library = root / "stdlib"
        first, second = library / "std/first.gti", library / "std/second.gti"
        write(library / "prelude.gti", "// Minimal installed prelude for this source-owner test.\n")
        write(first, "export module api; export int answer() { return 1; }\n")
        write(second, "export module api; export int answer() { return 2; }\n")
        public_map = library / "module-map.json"
        write(public_map, json.dumps({"modules": {"api": "std/first.gti"}}))
        entry = root / "app/main.gti"
        source = "import std.api;\nint main() { return answer(); }\n"
        write(entry, source)
        session = LspSession(sys.argv[1], environment={**os.environ, "GTI_STDLIB_PATH": str(library)})
        request_id = 30

        def definition():
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id,
                          "method": "textDocument/definition", "params": {
                              "textDocument": {"uri": uri},
                              "position": lsp_position(source, source.index("std.api") + 4)}})
            reply = session.receive_until(lambda message: message.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def remap(modules):
            write(public_map, json.dumps({"modules": modules}))
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": public_map.resolve().as_uri(), "type": 2}]}})
            return diagnostics_for(session, uri, 1)

        try:
            initialize(session, watched_files=True)
            uri = open_document(session, entry, source)
            assert diagnostics_for(session, uri, 1) == []
            assert definition()["uri"] == first.resolve().as_uri()
            assert remap({"api": "std/second.gti"}) == []
            assert definition()["uri"] == second.resolve().as_uri()
            removed = remap({})
            assert any(item.get("code") == "GTI-I0012" for item in removed), removed
            assert not definition(), "removed public map entry retained a stale definition"
        finally:
            session.close()


if __name__ == "__main__":
    main()
