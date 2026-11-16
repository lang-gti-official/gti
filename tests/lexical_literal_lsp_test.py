#!/usr/bin/env python3

"""Focused LSP coverage for numeric literal suffixes, digit separators and raw
string literals: semantic tokens, formatting and lexical diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lexical_literal_lsp_test.py /path/to/gti_lsp")

    source = (
        "int main() {\n"
        "  uint64_t big = 1_000_000u64;\n"
        "  float ratio = 2.5f32;\n"
        '  std::string_view pattern = R"re(\\d+ "x" // kept)re";\n'
        '  std::string_view block = R"(first\n'
        "second /* kept */)\";\n"
        "  int32_t R = 3;\n"
        "  return R - 3;\n"
        "}\n"
    )

    with tempfile.TemporaryDirectory(prefix="gti-literal-lsp-") as root:
        path = pathlib.Path(root) / "literals.gti"
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
            legend = initialization["capabilities"]["semanticTokensProvider"][
                "legend"
            ]["tokenTypes"]
            number_type = legend.index("number")
            string_type = legend.index("string")
            comment_type = legend.index("comment")
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
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

            def token_at(text):
                position = lsp_position(source, source.index(text))
                return tokens.get((position["line"], position["character"]))

            big = token_at("1_000_000u64")
            assert big == {"length": 12, "type": number_type, "modifiers": 0}, big
            ratio = token_at("2.5f32")
            assert ratio["type"] == number_type and ratio["length"] == 6, ratio
            raw = token_at('R"re(')
            assert raw["type"] == string_type, raw
            assert raw["length"] == len('R"re(\\d+ "x" // kept)re"'), raw
            # Neither `//` nor `/*` inside a raw string starts a comment.
            assert token_at("// kept") is None
            assert token_at("/* kept */") is None
            assert all(token["type"] != comment_type for token in tokens.values())
            first = token_at('R"(first')
            assert first["type"] == string_type and first["length"] == len(
                'R"(first'
            ), first
            continuation = tokens[(5, 0)]
            assert continuation["type"] == string_type, continuation
            assert continuation["length"] == len('second /* kept */)"')

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
            edits = session.receive_until(lambda message: message.get("id") == 3)[
                "result"
            ]
            formatted = edits[0]["newText"] if edits else source
            assert "1_000_000u64" in formatted and "2.5f32" in formatted
            assert 'R"re(\\d+ "x" // kept)re"' in formatted
            assert 'R"(first\nsecond /* kept */)"' in formatted

            invalid = "int main() {\n  uint64_t bad = 0x_ff;\n  return 0;\n}\n"
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
            separator = invalid.index("_ff")
            assert diagnostics[0]["code"] == "GTI-L0007", diagnostics
            assert diagnostics[0]["range"] == {
                "start": lsp_position(invalid, separator),
                "end": lsp_position(invalid, separator + 1),
            }, diagnostics

            unterminated = 'int main() {\n  auto text = R"x(open\n  return 0;\n}\n'
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 3},
                        "contentChanges": [{"text": unterminated}],
                    },
                }
            )
            diagnostics = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == uri
                and message["params"].get("version") == 3
            )["params"]["diagnostics"]
            opening = unterminated.index('R"')
            assert [item["code"] for item in diagnostics] == ["GTI-L0004"], diagnostics
            assert diagnostics[0]["range"] == {
                "start": lsp_position(unterminated, opening),
                "end": lsp_position(unterminated, opening + 2),
            }, diagnostics
        finally:
            session.close()


if __name__ == "__main__":
    main()
