#!/usr/bin/env python3

"""LSP coverage for the normative GTI source-text contract."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: source_text_lsp_test.py /path/to/gti_lsp")

    source = (
        "\ufeffint main() {\r\n"
        "  // café 😀\r\n"
        '  std::println("héllo 😀");\r\n'
        "  return 0;\r\n"
        "}\r\n"
    )

    with tempfile.TemporaryDirectory(prefix="gti-source-text-lsp-") as root:
        path = pathlib.Path(root) / "source-text.gti"
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
            type_type = token_types.index("type")
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
            assert tokens[(0, 1)] == {
                "length": 3,
                "type": type_type,
                "modifiers": 0,
            }, tokens

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
            assert formatted.startswith("\ufeff")
            assert "\r" not in formatted
            assert "café 😀" in formatted and "héllo 😀" in formatted

            invalid = "int main() {\r  return 0;\n}\n"
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
            offset = invalid.index("\r")
            assert diagnostic["code"] == "GTI-L0014", diagnostic
            assert diagnostic["range"] == {
                "start": lsp_position(invalid, offset),
                "end": lsp_position(invalid, offset + 1),
            }

            valid = "int main() {\r\n  return 0;\r\n}\r\n"
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 3},
                        "contentChanges": [{"text": valid}],
                    },
                }
            )
            cleared = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == uri
                and message["params"].get("version") == 3
            )
            assert not cleared["params"]["diagnostics"], cleared
        finally:
            session.close()


if __name__ == "__main__":
    main()
