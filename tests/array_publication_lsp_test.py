#!/usr/bin/env python3
"""Fresh-array acceptance and ownership errors use the compiler snapshot."""

from pathlib import Path
import sys
import tempfile

from lsp_smoke_test import LspSession


SOURCE = """class Pinned {
public:
  Pinned() {}
  Pinned(Pinned&) = delete;
  Pinned(Pinned&&) = delete;
};
int main() {
  Pinned original{};
  Pinned values[1] = {Pinned()};
  return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-array-publication-lsp-") as directory:
        path = Path(directory) / "main.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": SOURCE}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and
                    message["params"].get("version") == version)["params"]["diagnostics"]

            assert not diagnostics(1)
            for version, text in ((2, SOURCE.replace("{Pinned()}", "{original}")), (3, SOURCE)):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text}]}})
                errors = diagnostics(version)
                if version == 2:
                    error = next(item for item in errors if item.get("code") == "GTI-S2003")
                    assert "array element" in error["message"], error
                    assert error["range"]["start"]["line"] == 8, error
                else:
                    assert not errors, errors
        finally:
            session.close()
    print("fresh-array LSP acceptance, ownership diagnostic and recovery passed")


if __name__ == "__main__":
    main()
