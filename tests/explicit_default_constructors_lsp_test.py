#!/usr/bin/env python3
"""Publish the missing default-constructor contract without an unsafe fix-it."""
from pathlib import Path
import sys
import tempfile
from lsp_smoke_test import LspSession, initialize_session, open_document, full_change, lsp_position

with tempfile.TemporaryDirectory(prefix="gti-explicit-default-lsp-") as directory:
    path = Path(directory) / "construction.gti"
    source = "// 🙂\nstruct Resource { int state = 0; };\nint main() { Resource value{}; return 0; }\n"
    path.write_text(source)
    uri = path.resolve().as_uri()
    session = LspSession(sys.argv[1])
    try:
        initialize_session(session, {"textDocument": {"publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}})
        result = open_document(session, uri, source, expect_clean=False)["params"]
        errors = [d for d in result["diagnostics"] if d.get("code") == "GTI-S2012"]
        assert len(errors) == 1, result
        diagnostic = errors[0]
        assert "explicitly declared constructor" in diagnostic["message"], diagnostic
        assert "Resource() = default" in diagnostic["message"], diagnostic
        assert diagnostic["range"]["start"] == lsp_position(source, source.index("{}")), diagnostic
        related = diagnostic["relatedInformation"]
        assert any(r["location"]["uri"] == uri and r["location"]["range"]["start"] == lsp_position(source, source.index("Resource")) for r in related), related
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {"textDocument": {"uri": uri}, "range": diagnostic["range"], "context": {"diagnostics": [diagnostic]}}})
        actions = session.receive_until(lambda m: m.get("id") == 2)["result"]
        assert not actions, actions
        corrected = source.replace("int state = 0;", "int state = 0; Resource() = default;")
        session.send(full_change(uri, 2, corrected))
        publication = session.receive_until(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"].get("version") == 2)
        assert not publication["params"]["diagnostics"], publication
    finally:
        session.close()
