#!/usr/bin/env python3

"""LSP formatting keeps own-line comments on their own lines."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: comment_placement_lsp_test.py /path/to/gti_lsp")

    formatted = (
        "int answer() {\n"
        "    int y = 2;\n"
        "    // own line\n"
        "    /* own block */\n"
        "    return y; // trailing\n"
        "}\n"
        "// after function\n"
        "int main() {\n"
        "    return answer() - 2; /* trailing */\n"
        "}\n"
    )
    unformatted = (
        "int answer(){int y=2;\n"
        "// own line\n"
        "/* own block */\n"
        "return y;// trailing\n"
        "}\n"
        "// after function\n"
        "int main(){return answer()-2;/* trailing */}\n"
    )

    with tempfile.TemporaryDirectory(prefix="gti-comment-placement-lsp-") as root:
        path = pathlib.Path(root) / "comments.gti"
        path.write_text(formatted, encoding="utf-8")
        uri = path.resolve().as_uri()

        session = LspSession(sys.argv[1])
        try:
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"capabilities": {}},
                }
            )
            session.receive_until(lambda message: message.get("id") == 1)
            session.send(
                {"jsonrpc": "2.0", "method": "initialized", "params": {}}
            )
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": uri,
                            "languageId": "gti",
                            "version": 1,
                            "text": formatted,
                        }
                    },
                }
            )

            def diagnostics(version):
                return session.receive_until(
                    lambda message: message.get("method")
                    == "textDocument/publishDiagnostics"
                    and message["params"]["uri"] == uri
                    and message["params"].get("version") == version
                )["params"]["diagnostics"]

            def format_document(request_id):
                session.send(
                    {
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "method": "textDocument/formatting",
                        "params": {
                            "textDocument": {"uri": uri},
                            "options": {"tabSize": 4, "insertSpaces": True},
                        },
                    }
                )
                return session.receive_until(
                    lambda message: message.get("id") == request_id
                )["result"]

            assert diagnostics(1) == []
            assert format_document(2) == [], "formatted comments need no edit"

            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 2},
                        "contentChanges": [{"text": unformatted}],
                    },
                }
            )
            assert diagnostics(2) == []
            edits = format_document(3)
            assert len(edits) == 1, edits
            assert edits[0]["newText"] == formatted, edits[0]["newText"]
        finally:
            session.close()


if __name__ == "__main__":
    main()
