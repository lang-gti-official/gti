#!/usr/bin/env python3
"""Nested collection designators use shared frontend diagnostics and symbols."""
import pathlib
import sys
import tempfile
from lsp_smoke_test import without_include_advice, LspSession


def main():
    source = ('#include <std/vector>\n'
              'struct Command { int number = 0; std::vector<Command> children{};  public: Command() = default; };\n'
              'int main() { Command root = {.children = {{.number = 1}}}; return 0; }\n')
    with tempfile.TemporaryDirectory(prefix="gti-collection-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
            reply = session.receive_until(lambda m: m.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return without_include_advice(session.receive_until(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                and m["params"].get("version") == version)["params"]["diagnostics"])

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})
            assert not diagnostics(1)
            position = {"line": 2, "character": source.splitlines()[2].index(".number") + 1}
            params = {"textDocument": {"uri": uri}, "position": position}
            hover = request("textDocument/hover", params)
            assert "int32_t" in str(hover), (params, hover)
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == {"line": 1, "character": 21}, definition
            change(2, source.replace("{.number = 1}", "true"))
            errors = diagnostics(2)
            assert any(d.get("code") == "GTI-S2092" for d in errors), errors
            change(3, source.replace(".number = 1", ".number ="))
            assert diagnostics(3)
            change(4, source)
            assert not diagnostics(4)
            formatting_params = {"textDocument": {"uri": uri},
                                 "options": {"tabSize": 4, "insertSpaces": True}}
            edits = request("textDocument/formatting", formatting_params)
            formatted = edits[0]["newText"]
            assert ("    Command root = {\n"
                    "        .children = {\n"
                    "            {\n"
                    "                .number = 1\n"
                    "            }\n"
                    "        }\n"
                    "    };") in formatted, formatted
            change(5, formatted)
            assert not diagnostics(5)
            repeated = request("textDocument/formatting", formatting_params)
            assert not repeated or repeated[0]["newText"] == formatted, repeated
            (pathlib.Path(root) / ".gti-format").write_text("IndentWidth: 2\n")
            configured = request("textDocument/formatting", formatting_params)[0]["newText"]
            assert "  Command root = {\n    .children = {\n      {\n" in configured, configured
            constrained = (
                'class Batch<T> { public: Batch() {}\n'
                'static Batch<T> collection_init(uint64_t count) { return Batch<T>(); }\n'
                'static Batch<T> collection_insert(Batch<T> value, T element) '
                'requires std::integral<T> { return std::move(value); }\n'
                '};\nint main() { Batch<int> values = {1}; return 0; }\n')
            change(6, constrained)
            assert not diagnostics(6)
            invalid = constrained.replace('Batch<int> values = {1}', 'Batch<float> values = {}')
            change(7, invalid)
            errors = diagnostics(7)
            constraint = next((d for d in errors if d.get('code') == 'GTI-S2029'), None)
            assert constraint is not None, errors
            assert constraint['range']['start'] == {
                'line': 4, 'character': invalid.splitlines()[4].index('{}')}, constraint
            assert any('collection initialization hook' in item['message']
                       for item in constraint.get('relatedInformation', [])), constraint
            change(8, constrained)
            assert not diagnostics(8)
        finally:
            session.close()


if __name__ == "__main__":
    main()
