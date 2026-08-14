#!/usr/bin/env python3

"""Focused LSP coverage for GTI block comments."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def utf16_length(text):
    return len(text.encode("utf-16-le")) // 2


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: block_comment_lsp_test.py /path/to/gti_lsp")

    source = (
        "int main() {\n"
        "  /* first 😀 line\n"
        "     second line */\n"
        '  std::string_view text = "/* not a comment */";\n'
        "  return 0; /* trailing */\n"
        "}\n"
    )

    with tempfile.TemporaryDirectory(prefix="gti-block-comment-lsp-") as root:
        path = pathlib.Path(root) / "comments.gti"
        path.write_text(source, encoding="utf-8")
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
            initialization = session.receive_until(
                lambda message: message.get("id") == 1
            )["result"]
            token_types = initialization["capabilities"][
                "semanticTokensProvider"
            ]["legend"]["tokenTypes"]
            comment_type = token_types.index("comment")
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
                            "text": source,
                        }
                    },
                }
            )
            publication = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == uri
                and message["params"].get("version") == 1
            )
            assert not publication["params"]["diagnostics"], publication

            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/semanticTokens/full",
                    "params": {"textDocument": {"uri": uri}},
                }
            )
            tokens = semantic_tokens_by_position(
                session.receive_until(lambda message: message.get("id") == 2)[
                    "result"
                ]["data"]
            )
            opening = lsp_position(source, source.index("/* first"))
            first = tokens[(opening["line"], opening["character"])]
            assert first["type"] == comment_type
            assert first["length"] == utf16_length("/* first 😀 line")
            continuation = tokens[(2, 0)]
            assert continuation["type"] == comment_type
            assert continuation["length"] == utf16_length("     second line */")
            string_delimiter = lsp_position(
                source, source.index("/* not a comment */")
            )
            assert (
                string_delimiter["line"],
                string_delimiter["character"],
            ) not in tokens

            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/formatting",
                    "params": {
                        "textDocument": {"uri": uri},
                        "options": {"tabSize": 2, "insertSpaces": True},
                    },
                }
            )
            formatted = session.receive_until(
                lambda message: message.get("id") == 3
            )["result"][0]["newText"]
            assert "  /* first 😀 line\n   second line */\n" in formatted
            assert '"/* not a comment */"' in formatted
            assert "return 0; /* trailing */" in formatted

            invalid = "int main() {\n  /* unfinished\n  return 0;\n}\n"
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 2},
                        "contentChanges": [{"text": invalid}],
                    },
                }
            )
            diagnostics = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == uri
                and message["params"].get("version") == 2
            )["params"]["diagnostics"]
            assert len(diagnostics) == 1, diagnostics
            diagnostic = diagnostics[0]
            opening_offset = invalid.index("/*")
            assert diagnostic["code"] == "GTI-L0011", diagnostic
            assert diagnostic["message"] == "Unterminated block comment."
            assert diagnostic["range"] == {
                "start": lsp_position(invalid, opening_offset),
                "end": lsp_position(invalid, opening_offset + 2),
            }
        finally:
            session.close()


if __name__ == "__main__":
    main()
