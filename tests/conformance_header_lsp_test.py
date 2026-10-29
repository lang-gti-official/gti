#!/usr/bin/env python3
"""One conformance-header diagnostic, include help, UTF-16 spans and clearing."""

from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-conformance-header-") as directory:
        root = Path(directory).resolve()
        concept = root / "measure.gti"
        concept.write_text((
                               'export module measure;\n'
                               'export concept measurable<T> { int measure(T& item); bool is_empty(T& item); };\n'
                           ))
        (root / "middle.gti").write_text((
                                             'export module middle;\n'
                                             'import "measure.gti";\n'
                                         ))
        uri = (root / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        version = 0

        def publish(source):
            nonlocal version
            version += 1
            document = {"uri": uri, "version": version}
            if version == 1:
                document.update(languageId="gti", text=source)
                method = "textDocument/didOpen"
                params = {"textDocument": document}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": document, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)["params"]["diagnostics"]

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            for members in ("", "int measure(Rope& item) { return 1; }\n"
                                "bool is_empty(Rope& item) { return false; }\n"):
                source = ('import "middle.gti";\nstruct Rope {};\n'
                          '/* 😀 */ for concept measurable<Rope> {\n' + members + '};\n')
                errors = [item for item in publish(source) if item.get("severity") == 1]
                assert len(errors) == 1 and errors[0]["code"] == "GTI-S2024", errors
                error = errors[0]
                start = source.index("measurable")
                assert error["range"] == {
                    "start": lsp_position(source, start),
                    "end": lsp_position(source, start + len("measurable"))}, error
                assert any(item["location"]["uri"] == concept.as_uri()
                           for item in error.get("relatedInformation", [])), error
                assert 'measure.gti' in error["message"], error
                unknown = source.replace("measurable<Rope>", "missing<Rope>")
                errors = [item for item in publish(unknown) if item.get("severity") == 1]
                assert len(errors) == 1 and errors[0]["code"] == "GTI-S2123", errors
            valid = source.replace('"middle.gti"', '"measure.gti"')
            assert (publish(valid) == []), "correcting the include clears the error"
        finally:
            session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
