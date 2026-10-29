#!/usr/bin/env python3
"""Snippet negotiation, exact edits and compilable GTI template expansions."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def expand(text):
    """Expand the catalog's tab stops/defaults as an editor would."""
    values = {}

    def placeholder(match):
        index = match[1]
        values[index] = match[2] or ""
        return values[index]

    text = re.sub(r"\$\{(\d+)(?::([^}]*))?\}", placeholder, text)
    return re.sub(r"\$(\d+)", lambda match: values.get(match[1], ""), text)


def run(executable, compiler, root, support):
    session = LspSession(executable)
    seq = version = 0
    path = root / "editor.gti"
    uri = path.as_uri()

    def request(method, params):
        nonlocal seq
        seq += 1
        session.send({"jsonrpc": "2.0", "id": seq, "method": method, "params": params})
        reply = session.receive_until(lambda message: message.get("id") == seq)
        assert "error" not in reply, reply
        return reply["result"]

    def complete(marked):
        nonlocal version
        offset = marked.index("|")
        source = marked.replace("|", "", 1)
        version += 1
        if version == 1:
            params = {"textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}
            method = "textDocument/didOpen"
        else:
            params = {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}
            method = "textDocument/didChange"
        session.send({"jsonrpc": "2.0", "method": method, "params": params})
        session.receive_until(lambda message: message.get("method") == "textDocument/publishDiagnostics"
                              and message["params"].get("uri") == uri
                              and message["params"].get("version") == version)
        params = {"textDocument": {"uri": uri}, "position": lsp_position(source, offset)}
        result = request("textDocument/completion", params)
        return source, result["items"]

    try:
        request("initialize", {"capabilities": {"textDocument": {"completion": {
            "completionItem": {"snippetSupport": support}}}}})
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        if not support:
            _, items = complete("cl|")
            assert not any(item.get("kind") == 15 for item in items), items
            assert not any("${" in item.get("textEdit", {}).get("newText", "") for item in items), items
            _, items = complete("int32_t example(int32_t input) { return input; }\nint32_t f() { return exa|; }")
            example = next(item for item in items if item["label"] == "example")
            assert example["textEdit"]["newText"] == "example" and example.get("insertTextFormat", 1) == 1
            return

        total = 0
        contexts = (
            "|\nint main() { return 0; }\n",
            "class Widget { public:\n|\n};\nint main() { return 0; }\n",
            "import std.vector;\nvoid f() {\n  bool condition = false; int32_t count = 0; int32_t value = 0;\n  std::vector<int32_t> values = std::vector<int32_t>();\n  |\n}\nint main() { f(); return 0; }\n",
        )
        for context_index, marked in enumerate(contexts):
            source, items = complete(marked)
            snippets = [item for item in items if item.get("kind") == 15]
            assert snippets, (marked, items)
            assert any(item["label"] == "macro" for item in snippets) == (context_index == 0), (marked, snippets)
            for item in snippets:
                assert item["insertTextFormat"] == 2, item
                edit = item["textEdit"]
                assert edit["range"]["start"] == edit["range"]["end"], edit
                offset = marked.index("|")
                program = source[:offset] + expand(edit["newText"]) + source[offset:]
                output = root / f"expanded-{total}.gti"
                output.write_text(program, encoding="utf-8")
                result = subprocess.run([compiler, str(output), "-o", str(root / "expanded-app")], capture_output=True, text=True)
                assert result.returncode == 0, (item["label"], program, result.stdout, result.stderr)
                subprocess.run([str(root / "expanded-app")], check=True, timeout=30,
                               capture_output=True, text=True)
                total += 1
        assert total >= 20, total

        source, items = complete("/* 😀 */ cl|utter")
        item = next(item for item in items if item["label"] == "class" and item.get("kind") == 15)
        assert item["textEdit"]["range"] == {
            "start": lsp_position(source, source.index("clutter")),
            "end": lsp_position(source, len(source)),
        }, item
        assert "$1()" in item["textEdit"]["newText"], item
        source, items = complete("/* 😀 */ ma|cro")
        macro = next(item for item in items if item["label"] == "macro" and item.get("kind") == 15)
        assert macro["textEdit"]["newText"] == "#macro ${1:name}(${2:int value}) {\n\t$0\n}", macro
        assert macro["textEdit"]["range"] == {
            "start": lsp_position(source, source.index("macro")),
            "end": lsp_position(source, len(source)),
        }, macro
        _, items = complete("namespace app { ma| }")
        assert any(item["label"] == "macro" and item.get("kind") == 15 for item in items), items
        _, items = complete("class Outer { struct Inner { ct| }; };")
        ctor = next(item for item in items if item["label"] == "ctor")
        assert ctor["textEdit"]["newText"].startswith("Inner("), ctor
        _, items = complete("void f() { if| }")
        assert any(item["label"] == "if" and item.get("kind") == 15 for item in items)
        for marked in ("// cl|", "/* cl|", '"cl|"', "void f() { f(cl|); }", "void f() { this.cl|; }"):
            _, items = complete(marked)
            assert not any(item.get("kind") == 15 for item in items), (marked, items)

        # Closing retires the former owner/context; reopening uses new text.
        session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
        version = 0
        _, items = complete("struct Reopened { dt| };")
        dtor = next(item for item in items if item["label"] == "dtor")
        assert dtor["textEdit"]["newText"].startswith("~Reopened("), dtor
        print(f"Snippet-capable client: {total} expanded templates compile")
    finally:
        session.close()


def main():
    with tempfile.TemporaryDirectory(prefix="gti-lsp-snippets-") as directory:
        for support in (True, False):
            run(sys.argv[1], sys.argv[2], Path(directory), support)


if __name__ == "__main__":
    main()
