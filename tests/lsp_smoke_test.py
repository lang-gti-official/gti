#!/usr/bin/env python3

"""Protocol smoke test for the GTI language server."""

import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile
import time


def frame(message):
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload


def decode_messages(output):
    messages = []
    offset = 0
    while offset < len(output):
        header_end = output.find(b"\r\n\r\n", offset)
        if header_end < 0:
            raise AssertionError("LSP response has an incomplete header")
        headers = output[offset:header_end].decode("ascii").split("\r\n")
        content_length = None
        for header in headers:
            name, value = header.split(":", 1)
            if name.lower() == "content-length":
                content_length = int(value.strip())
        if content_length is None:
            raise AssertionError("LSP response is missing Content-Length")
        payload_start = header_end + 4
        payload_end = payload_start + content_length
        messages.append(json.loads(output[payload_start:payload_end]))
        offset = payload_end
    return messages


def lsp_position(source, offset):
    prefix = source[:offset]
    line = prefix.count("\n")
    line_text = prefix.rsplit("\n", 1)[-1]
    character = len(line_text.encode("utf-16-le")) // 2
    return {"line": line, "character": character}


def semantic_tokens_by_position(data):
    tokens = {}
    line = 0
    character = 0
    for index in range(0, len(data), 5):
        delta_line, delta_start, length, token_type, modifiers = data[
            index : index + 5
        ]
        if delta_line:
            line += delta_line
            character = delta_start
        else:
            character += delta_start
        tokens[(line, character)] = {
            "length": length,
            "type": token_type,
            "modifiers": modifiers,
        }
    return tokens


class LspSession:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.buffer = b""

    def send(self, message):
        self.process.stdin.write(frame(message))
        self.process.stdin.flush()

    def receive_until(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        received = []
        while time.monotonic() < deadline:
            header_end = self.buffer.find(b"\r\n\r\n")
            if header_end >= 0:
                headers = self.buffer[:header_end].decode("ascii").split("\r\n")
                content_length = next(
                    int(value.strip())
                    for name, value in (header.split(":", 1) for header in headers)
                    if name.lower() == "content-length"
                )
                payload_start = header_end + 4
                payload_end = payload_start + content_length
                if len(self.buffer) >= payload_end:
                    message = json.loads(self.buffer[payload_start:payload_end])
                    self.buffer = self.buffer[payload_end:]
                    received.append(message)
                    if predicate(message):
                        return message
                    continue

            remaining = deadline - time.monotonic()
            ready, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], max(remaining, 0)
            )
            if not ready:
                break
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                break
            self.buffer += chunk

        stderr = ""
        ready, _, _ = select.select([self.process.stderr.fileno()], [], [], 0)
        if ready:
            stderr = os.read(self.process.stderr.fileno(), 65536).decode(
                errors="replace"
            )
        raise AssertionError(
            f"timed out waiting for LSP message; received {received}; stderr: {stderr}"
        )

    def close(self):
        self.send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None})
        self.receive_until(lambda message: message.get("id") == 99)
        self.send({"jsonrpc": "2.0", "method": "exit", "params": None})
        self.process.wait(timeout=5)
        if self.process.returncode != 0:
            raise AssertionError(
                f"gti_lsp exited with {self.process.returncode}: "
                f"{self.process.stderr.read().decode(errors='replace')}"
            )


def test_inheritance_tooling(executable, root):
    source = (
        "interface Renderable{int render(int frame)=0;};\n"
        "class Base{public:virtual int tick(int frame){return frame;}};\n"
        "class Sprite:public Base,public Renderable{public:"
        "int tick(int frame)override{return frame;}"
        "int render(int frame)override{return this.tick(frame);}};\n"
        "int invoke(Renderable& value){return value.render(1);}\n"
        "interface RangeIteratorContract<T>{T& operator*()=0;"
        "void operator++()mut=0;};\n"
        "class RangeIterator:public RangeIteratorContract<int>{"
        "mut int current=0;public:int& operator*()override{return this.current;}"
        "void operator++()mut override{this.current++;}"
        "bool operator!=(RangeIterator& other){"
        "return this.current!=other.current;}};\n"
        "class Range{public:RangeIterator begin(){return RangeIterator();}"
        "RangeIterator end(){return RangeIterator();}};\n"
        "int sum(Range& values){mut int result=0;"
        "for(auto& value:values){result+=value;}return result;}\n"
    )
    path = root / "inheritance-tooling.gti"
    path.write_text(source, encoding="utf-8")
    uri = path.resolve().as_uri()

    session = LspSession(executable)
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
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
            and not message["params"]["diagnostics"]
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        data = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]["data"]
        tokens = semantic_tokens_by_position(data)

        def token_type(text, start=0):
            position = lsp_position(source, source.index(text, start))
            return tokens[(position["line"], position["character"])]["type"]

        assert token_type("interface") == 0
        assert token_type("Renderable") == 4
        sprite_base = source.index("Base", source.index("class Sprite"))
        assert token_type("Base", sprite_base) == 4
        assert token_type("virtual") == 0
        assert token_type("override") == 0
        render_call = source.index("render", source.index("return value.render"))
        assert token_type("render", render_call) == 6
        range_binding = source.index("value", source.index("for(auto& value"))
        assert token_type("value", range_binding) == 7
        range_colon = source.index(":", source.index("for(auto& value"))
        colon_position = lsp_position(source, range_colon)
        assert tokens[(colon_position["line"], colon_position["character"])][
            "type"
        ] == 12

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
        formatted = session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ][0]["newText"]
        assert "interface Renderable {" in formatted
        assert "class Sprite : public Base, public Renderable {" in formatted
        assert "int render(int frame) override {" in formatted
        assert "void operator++() mut override {" in formatted
        assert "for (auto & value : values) {" in formatted

        invalid_source = "interface Invalid { int state = 0; };\n"
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": invalid_source}],
                },
            }
        )
        diagnostics = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
            and bool(message["params"]["diagnostics"])
        )["params"]["diagnostics"]
        assert any(item.get("code") == "GTI-S2041" for item in diagnostics)
    finally:
        session.close()


def test_unsaved_dependency_reanalysis(executable, root):
    library_path = root / "overlay-library.gti"
    root_path = root / "overlay-root.gti"
    disk_source = 'int overlay_value = "stale disk text";\n'
    valid_source = "int overlay_value = 1;\n"
    invalid_buffer_source = 'int overlay_value = "unsaved buffer text";\n'
    root_source = (
        '#include "overlay-library.gti"\n'
        "int main() { return overlay_value; }\n"
    )
    library_path.write_text(disk_source, encoding="utf-8")
    root_path.write_text(root_source, encoding="utf-8")
    library_uri = library_path.resolve().as_uri()
    root_uri = root_path.resolve().as_uri()

    session = LspSession(executable)
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
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": root_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": root_source,
                    }
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == library_uri
            and bool(message["params"]["diagnostics"])
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": library_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": disk_source,
                    }
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": library_uri, "version": 2},
                    "contentChanges": [{"text": valid_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == library_uri
            and message["params"].get("version") == 2
            and not message["params"]["diagnostics"]
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": library_uri, "version": 3},
                    "contentChanges": [{"text": invalid_buffer_source}],
                },
            }
        )
        unsaved_diagnostic = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == library_uri
            and message["params"].get("version") == 3
            and bool(message["params"]["diagnostics"])
        )
        assert any(
            diagnostic["code"] == "GTI-S2003"
            for diagnostic in unsaved_diagnostic["params"]["diagnostics"]
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": library_uri, "version": 4},
                    "contentChanges": [{"text": valid_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == library_uri
            and message["params"].get("version") == 4
            and not message["params"]["diagnostics"]
        )
    finally:
        session.close()


def test_direct_dependency_visibility(executable, root):
    leaf_path = root / "visibility-leaf.gti"
    branch_path = root / "visibility-branch.gti"
    root_path = root / "visibility-root.gti"
    leaf_source = "int visibility_leaf() { return 1; }\n"
    branch_source = (
        '#include "visibility-leaf.gti"\n'
        "int visibility_branch() { return visibility_leaf(); }\n"
    )
    root_source = (
        '#include "visibility-branch.gti"\n'
        "int main() { return visibility_leaf(); }\n"
    )
    leaf_path.write_text(leaf_source, encoding="utf-8")
    branch_path.write_text(branch_source, encoding="utf-8")
    root_path.write_text(root_source, encoding="utf-8")
    root_uri = root_path.resolve().as_uri()
    leaf_uri = leaf_path.resolve().as_uri()

    session = LspSession(executable)
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
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": root_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": root_source,
                    }
                },
            }
        )
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == root_uri
            and message["params"].get("version") == 1
        )["params"]
        diagnostic = next(
            item
            for item in publication["diagnostics"]
            if item.get("code") == "GTI-S2024"
        )
        assert '#include "visibility-leaf.gti"' in diagnostic["message"]
        assert diagnostic["relatedInformation"][0]["location"]["uri"] == leaf_uri
    finally:
        session.close()


def test_missing_include_and_format_config(executable, root):
    missing_root = root / "missing-include-root.gti"
    missing_dependency = root / "missing-include-never-created.gti"
    missing_dependency.unlink(missing_ok=True)
    missing_source = (
        '#include "missing-include-never-created.gti"\n'
        "int main() { return 0; }\n"
    )
    missing_root.write_text(missing_source, encoding="utf-8")
    missing_uri = missing_root.resolve().as_uri()

    session = LspSession(executable)
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
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": missing_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": missing_source,
                    }
                },
            }
        )
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == missing_uri
            and any(
                diagnostic.get("code") == "GTI-I0008"
                for diagnostic in message["params"]["diagnostics"]
            )
        )
        assert publication["params"]["diagnostics"][0]["code"] == "GTI-I0008"
        assert not missing_dependency.exists()
    finally:
        session.close()

    config_root = root / "format-config"
    source_root = config_root / "nested"
    source_root.mkdir(parents=True)
    (config_root / ".gti-format").write_text(
        "BasedOnStyle: GTI\n"
        "IndentWidth: 4\n"
        "UseTab: Never\n"
        "BreakBeforeBraces: Allman\n"
        "SpaceBeforeParens: Never\n"
        "IndentCaseLabels: true\n"
        "AccessModifierOffset: -4\n"
        "MaxEmptyLinesToKeep: 0\n"
        "SpacesBeforeTrailingComments: 3\n"
        "SpaceBeforeAssignmentOperators: false\n"
        "ReferenceAlignment: Right\n",
        encoding="utf-8",
    )
    format_path = source_root / "configured.gti"
    format_source = (
        "class Box{public:int value=0;};uint32 legacy=0;"
        "int inspect(int& value,Box& box){int bits=value&value;// note\n"
        "switch(bits){case 0:return 0;default:if(bits>0&&true||false){"
        "return bits;}}return bits;}"
    )
    format_path.write_text(format_source, encoding="utf-8")
    format_uri = format_path.resolve().as_uri()

    session = LspSession(executable)
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
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": format_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": format_source,
                    }
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/formatting",
                "params": {
                    "textDocument": {"uri": format_uri},
                    "options": {"tabSize": 2, "insertSpaces": True},
                },
            }
        )
        response = session.receive_until(lambda message: message.get("id") == 2)
        formatted = response["result"][0]["newText"]
        assert "class Box\n{\npublic:\n    int value= 0;\n};" in formatted
        assert "uint32_t legacy= 0;" in formatted
        assert "int inspect(int &value, Box &box)\n{" in formatted
        assert "int &value" in formatted
        assert "Box &box" in formatted
        assert "int bits= value & value;   // note" in formatted
        assert "\n        case 0:\n            return 0;" in formatted
        assert "if(bits > 0 && true || false)\n" in formatted
    finally:
        session.close()


def test_semantic_hover(executable, root):
    source = (
        "uint64_t choose(uint64_t value) { return value; }\n"
        "float choose(float value) { return value; }\n"
        'int main() { std::print("🙂"); auto inferred = '
        "choose(uint64_t(1)); return 0; }\n"
    )
    path = root / "semantic-hover.gti"
    path.write_text(source, encoding="utf-8")
    uri = path.resolve().as_uri()

    session = LspSession(executable)
    try:
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "capabilities": {
                        "textDocument": {
                            "hover": {"contentFormat": ["markdown", "plaintext"]}
                        }
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )
        assert initialization["result"]["capabilities"]["hoverProvider"] is True
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
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
            and not message["params"]["diagnostics"]
        )

        call_offset = source.index("choose(uint64_t", source.index("int main"))
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, call_offset + 1),
                },
            }
        )
        selected = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert selected["contents"]["kind"] == "markdown"
        assert "```gti\nuint64_t choose(uint64_t value)\n```" in selected[
            "contents"
        ]["value"]
        assert "float choose" not in selected["contents"]["value"]
        assert selected["range"] == {
            "start": lsp_position(source, call_offset),
            "end": lsp_position(source, call_offset + len("choose")),
        }

        auto_offset = source.index("auto inferred")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, auto_offset + 1),
                },
            }
        )
        inferred = session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ]
        assert inferred["contents"]["value"].startswith("```gti\nuint64_t\n```")

        emoji_offset = source.index("🙂")
        split_surrogate = lsp_position(source, emoji_offset)
        split_surrogate["character"] += 1
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": split_surrogate,
                },
            }
        )
        assert session.receive_until(lambda message: message.get("id") == 4)[
            "result"
        ] is None
    finally:
        session.close()


def test_semantic_definition(executable, root):
    dependency_source = (
        "namespace math {\n"
        "uint64_t choose(uint64_t value) { return value; }\n"
        "float choose(float value) { return value; }\n"
        "}\n"
        "class Box {\n"
        "  int value = 0;\n"
        "public:\n"
        "  Box(int initial) : value(initial) {}\n"
        "};\n"
        "struct Point { int x = 0; };\n"
        "enum class Mode { Active };\n"
    )
    dependency_path = root / "definitions.gti"
    dependency_path.write_text(dependency_source, encoding="utf-8")
    source = (
        '#include "definitions.gti"\n'
        "namespace calc = math;\n"
        "int main() {\n"
        "  uint64_t result = calc::choose(uint64_t(1));\n"
        "  Box box = Box(1);\n"
        "  Point point = Point();\n"
        "  Mode mode = Mode::Active;\n"
        "  return 0;\n"
        "}\n"
    )
    path = root / "semantic-definition.gti"
    path.write_text(source, encoding="utf-8")
    uri = path.resolve().as_uri()

    session = LspSession(executable)
    try:
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"capabilities": {}},
            }
        )
        capabilities = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]["capabilities"]
        assert capabilities["definitionProvider"] is True
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
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
            and not message["params"]["diagnostics"]
        )

        def definition(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        call = source.index("choose")
        selected = definition(2, call + 1)
        choose_declaration = dependency_source.index("choose(uint64_t")
        assert selected == {
            "uri": dependency_path.resolve().as_uri(),
            "range": {
                "start": lsp_position(dependency_source, choose_declaration),
                "end": lsp_position(
                    dependency_source, choose_declaration + len("choose")
                ),
            },
        }

        alias_use = source.index("calc::")
        namespace_definition = definition(3, alias_use + 1)
        alias_declaration = source.index("calc", source.index("namespace calc"))
        assert namespace_definition["uri"] == path.resolve().as_uri()
        assert namespace_definition["range"]["start"] == lsp_position(
            source, alias_declaration
        )

        alias_target = source.index("math", source.index("namespace calc"))
        target_definition = definition(4, alias_target + 1)
        namespace_declaration = dependency_source.index("math")
        assert target_definition["uri"] == dependency_path.resolve().as_uri()
        assert target_definition["range"]["start"] == lsp_position(
            dependency_source, namespace_declaration
        )

        class_use = source.index("Box box")
        class_definition = definition(5, class_use + 1)
        class_declaration = dependency_source.index(
            "Box", dependency_source.index("class")
        )
        assert class_definition["range"]["start"] == lsp_position(
            dependency_source, class_declaration
        )

        construction = source.index("Box(1)")
        constructor_definition = definition(6, construction + 1)
        constructor_declaration = dependency_source.index("Box(int initial)")
        assert constructor_definition["range"]["start"] == lsp_position(
            dependency_source, constructor_declaration
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        semantic_data = session.receive_until(
            lambda message: message.get("id") == 7
        )["result"]["data"]
        semantic_tokens = semantic_tokens_by_position(semantic_data)
        namespace_position = lsp_position(source, alias_use)
        call_position = lsp_position(source, call)
        class_position = lsp_position(source, class_use)
        construction_position = lsp_position(source, construction)
        struct_use = source.index("Point point")
        enum_use = source.index("Mode mode")
        enumerator_use = source.index("Active")
        struct_position = lsp_position(source, struct_use)
        enum_position = lsp_position(source, enum_use)
        enumerator_position = lsp_position(source, enumerator_use)
        assert semantic_tokens[
            (namespace_position["line"], namespace_position["character"])
        ]["type"] == 3
        assert semantic_tokens[
            (call_position["line"], call_position["character"])
        ]["type"] == 5
        assert semantic_tokens[
            (class_position["line"], class_position["character"])
        ]["type"] == 4
        assert semantic_tokens[
            (construction_position["line"], construction_position["character"])
        ]["type"] == 6
        assert semantic_tokens[
            (struct_position["line"], struct_position["character"])
        ]["type"] == 17
        assert semantic_tokens[
            (enum_position["line"], enum_position["character"])
        ]["type"] == 18
        assert semantic_tokens[
            (enumerator_position["line"], enumerator_position["character"])
        ]["type"] == 16
    finally:
        session.close()


def test_semantic_completion_and_parameter_tokens(executable, root):
    source = (
        "static int file_value = 1;\n"
        "class Registry { public: static int answer = 42; "
        "static int current() { return answer; } };\n"
        "namespace math { uint64_t power(uint64_t base, uint64_t exponent) { "
        "return base + exponent; } float power(float base, float exponent) { "
        "return base + exponent; } }\n"
        "int choose(int left, int right) { int local = left; "
        "return left + right + local; }\n"
        "int main() { int observed = Registry::current() + file_value; "
        "return choose(1, 2); }\n"
    )
    path = root / "semantic-completion.gti"
    path.write_text(source, encoding="utf-8")
    uri = path.resolve().as_uri()

    session = LspSession(executable)
    try:
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "capabilities": {
                        "workspace": {
                            "semanticTokens": {"refreshSupport": True}
                        },
                        "textDocument": {
                            "completion": {
                                "completionItem": {"snippetSupport": True}
                            }
                        },
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]["capabilities"]
        completion_provider = initialization["completionProvider"]
        assert completion_provider["triggerCharacters"] == [".", ">", ":"]
        assert completion_provider["resolveProvider"] is False
        assert initialization["semanticTokensProvider"]["legend"][
            "tokenModifiers"
        ] == [
            "declaration",
            "definition",
            "readonly",
            "defaultLibrary",
            "functionScope",
            "static",
        ]

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
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
            and not message["params"]["diagnostics"]
        )
        refresh = session.receive_until(
            lambda message: message.get("method")
            == "workspace/semanticTokens/refresh"
        )
        session.send(
            {"jsonrpc": "2.0", "id": refresh["id"], "result": None}
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        semantic_data = session.receive_until(
            lambda message: message.get("id") == 2
        )["result"]["data"]
        semantic_tokens = semantic_tokens_by_position(semantic_data)
        parameter_declaration = source.index("left", source.index("choose"))
        parameter_reference = source.index("left + right")
        local_reference = source.index("local;", source.index("return left"))
        declaration_position = lsp_position(source, parameter_declaration)
        reference_position = lsp_position(source, parameter_reference)
        local_position = lsp_position(source, local_reference)
        declaration_token = semantic_tokens[
            (declaration_position["line"], declaration_position["character"])
        ]
        reference_token = semantic_tokens[
            (reference_position["line"], reference_position["character"])
        ]
        local_token = semantic_tokens[
            (local_position["line"], local_position["character"])
        ]
        assert declaration_token["type"] == 8
        assert declaration_token["modifiers"] & 1
        assert declaration_token["modifiers"] & 16
        assert reference_token["type"] == 8
        assert reference_token["modifiers"] & 16
        assert not reference_token["modifiers"] & 1
        assert local_token["type"] == 7
        assert local_token["modifiers"] & 16
        static_global = lsp_position(source, source.index("file_value"))
        static_field = lsp_position(source, source.index("answer"))
        static_method = lsp_position(source, source.index("current"))
        assert semantic_tokens[
            (static_global["line"], static_global["character"])
        ]["modifiers"] & 32
        assert semantic_tokens[
            (static_field["line"], static_field["character"])
        ]["modifiers"] & 32
        assert semantic_tokens[
            (static_method["line"], static_method["character"])
        ]["modifiers"] & 32
        scope_operator = lsp_position(source, source.index("::"))
        assert (
            scope_operator["line"],
            scope_operator["character"],
        ) not in semantic_tokens

        local_source = source.replace("int local = left;", "int local = left; int sink = loc;")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": local_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )
        local_prefix = local_source.index("loc;", local_source.index("sink"))
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(local_source, local_prefix + 3),
                },
            }
        )
        local_completion = session.receive_until(
            lambda message: message.get("id") == 3
        )["result"]
        local_item = next(
            item for item in local_completion["items"] if item["label"] == "local"
        )
        assert local_item["kind"] == 6
        assert local_item["detail"] == "int32_t local"
        assert local_item["textEdit"] == {
            "range": {
                "start": lsp_position(local_source, local_prefix),
                "end": lsp_position(local_source, local_prefix + 3),
            },
            "newText": "local",
        }

        namespace_source = source.replace(
            "return choose(1, 2);", "uint64_t result = math::po; return 0;"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": namespace_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
        )
        namespace_prefix = namespace_source.index("po;", namespace_source.index("math::"))
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(
                        namespace_source, namespace_prefix + 2
                    ),
                },
            }
        )
        namespace_completion = session.receive_until(
            lambda message: message.get("id") == 4
        )["result"]
        power_items = [
            item
            for item in namespace_completion["items"]
            if item["label"] == "power"
        ]
        assert len(power_items) == 2
        assert all(item["kind"] == 3 for item in power_items)
        assert any("math::power" in item["detail"] for item in power_items)
        assert all(item["insertTextFormat"] == 2 for item in power_items)
        assert any("${1:base}" in item["textEdit"]["newText"] for item in power_items)

        class_source = source.replace("Registry::current()", "Registry::cu")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 4},
                    "contentChanges": [{"text": class_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 4
        )
        class_prefix = class_source.index("cu", class_source.index("Registry::cu"))
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(class_source, class_prefix + 2),
                },
            }
        )
        class_completion = session.receive_until(
            lambda message: message.get("id") == 5
        )["result"]
        current_item = next(
            item
            for item in class_completion["items"]
            if item["label"] == "current"
        )
        assert current_item["kind"] == 2
        assert current_item["detail"].startswith("static int32_t Registry::current")
    finally:
        session.close()


def test_diagnostic_code_actions(executable, root):
    source = 'int main() { std::print("🙂"); int value = 1 return 0; }\n'
    corrected_source = 'int main() { std::print("🙂"); int value = 1; return 0; }\n'
    path = root / "code-actions.gti"
    path.write_text(source, encoding="utf-8")
    uri = path.resolve().as_uri()

    session = LspSession(executable)
    try:
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "capabilities": {
                        "workspace": {
                            "workspaceEdit": {"documentChanges": True}
                        },
                        "textDocument": {
                            "codeAction": {
                                "codeActionLiteralSupport": {
                                    "codeActionKind": {
                                        "valueSet": ["quickfix"]
                                    }
                                }
                            }
                        },
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        assert initialization["capabilities"]["codeActionProvider"] == {
            "codeActionKinds": ["quickfix"],
            "resolveProvider": False,
        }
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
            and bool(message["params"]["diagnostics"])
        )["params"]
        diagnostic = next(
            item
            for item in publication["diagnostics"]
            if item.get("data", {}).get("fixes")
        )
        fix = diagnostic["data"]["fixes"][0]
        assert fix["range"]["start"] == lsp_position(
            source, source.index("return")
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": diagnostic["range"],
                    "context": {
                        "diagnostics": [diagnostic],
                        "only": ["quickfix"],
                        "triggerKind": 1,
                    },
                },
            }
        )
        actions = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert len(actions) == 1, actions
        action = actions[0]
        assert action["title"] == fix["message"]
        assert action["kind"] == "quickfix"
        assert action["isPreferred"] is True
        assert action["diagnostics"][0]["code"] == diagnostic["code"]
        document_edit = action["edit"]["documentChanges"][0]
        assert document_edit["textDocument"] == {"uri": uri, "version": 1}
        assert document_edit["edits"] == [
            {"range": fix["range"], "newText": fix["replacement"]}
        ]

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": corrected_source}],
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": diagnostic["range"],
                    "context": {
                        "diagnostics": [diagnostic],
                        "only": ["quickfix"],
                    },
                },
            }
        )
        assert session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ] == []
    finally:
        session.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_smoke_test.py /path/to/gti_lsp")

    expected_version = (
        pathlib.Path(__file__).resolve().parent.parent / "VERSION"
    ).read_text(encoding="utf-8").strip()
    version = subprocess.run(
        [sys.argv[1], "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
        timeout=5,
    )
    assert version.returncode == 0, version.stderr
    assert version.stdout.strip() == f"gti_lsp {expected_version}"

    directory = tempfile.TemporaryDirectory(prefix="gti-lsp-test-")
    root = pathlib.Path(directory.name)
    test_semantic_hover(sys.argv[1], root)
    test_semantic_definition(sys.argv[1], root)
    test_semantic_completion_and_parameter_tokens(sys.argv[1], root)
    test_diagnostic_code_actions(sys.argv[1], root)
    library_source = (
        "T identity<T>(T value) { return value; }\n"
        'int dependency_value = "bad";\n'
    )
    library_path = root / "library.gti"
    library_path.write_text(library_source, encoding="utf-8")
    uri = (root / "lsp-smoke.gti").as_uri()
    stress_uri = (root / "lsp-stress.gti").as_uri()
    source = (
        '#include "library.gti"\n'
        "#include <std/array>\n"
        '#if target.os == "never"\n'
        "int inactive() { return missing_name; }\n"
        '#error "inactive target"\n'
        "#endif\n"
        "using EntityId=uint64_t;\n"
        "namespace engine { namespace graphics { void render() {} } }\n"
        "namespace gfx = engine::graphics;\n"
        'extern "C" { int32_t native_close(int32_t descriptor); }\n'
        "void raw_write(uint8_t* writable, const uint8_t* readable) { "
        "unsafe { uint8_t* alias = &writable[0]; *alias = readable[0]; } }\n"
        "enum class Stage : uint8_t { Boot, Running = 4, };\n"
        "class Box<T> { T value; public: Box(T value) : value(value) {} "
        "T& get() { return this.value; } };\n"
        "class StaticArray<T, uint64_t N> { T values[N] = {}; public: "
        "uint64_t size() { return N; } };\n"
        "class FormattingProbe<T> { public: // Generic iterator access.\n"
        "Box<T> begin(); };\n"
        "class ReadOnlyArrayReceiver { mut int slots[1] = {0}; public: "
        "void write() { slots[0] = 1; } };\n"
        "struct Pixel { public: mut int x; Pixel(int x) : x(x) {} "
        "void reset() mut { this.x = 0; } "
        "~Pixel() { this.reset(); } private: int y = 0; };\n"
        "class Handle { mut Pixel pixel = Pixel(1); mut int value = 0; public: "
        "Pixel& operator->() { return this.pixel; } "
        "mut Pixel& operator->() mut { return this.pixel; } "
        "int& operator*() { return this.value; } "
        "mut int& operator*() mut { return this.value; } "
        "int& operator[](uint64_t index) { return this.value; } "
        "mut int& operator[](uint64_t index) mut { return this.value; } "
        "bool operator==(nullptr_t other) { return false; } "
        "bool operator!=(nullptr_t other) { return true; } "
        "uint64_t operator()(uint64_t value) { return value; } "
        "int self_identifier() { int self = 1; return self; } "
        "operator bool() { return true; } };\n"
        "struct Shade { int value = 0; Shade(int value) : value(value) {} "
        "Shade(bool reset) {} };\n"
        "int inspect_pixel(Pixel& pixel) { return pixel.x; }\n"
        "expected<int, int> calculate(bool fail) { "
        "if (fail) { return unexpected(1); } return 2; }\n"
        "uint64_t overloaded(uint64_t value) { return value; }\n"
        "float overloaded(float value) { return value; }\n"
        "T constrained<std::numeric T>(T value) { return value; }\n"
        "int incomplete(bool value) { if (value) { return 1; } }\n"
        "void consume<Args...>(Args... values) {}\n"
        "void relay<Args...>(Args... values) { consume(values...); }\n"
        'int main() { std::print("\U0001F642"); gfx::render(); '
        "Stage stage = Stage::Boot; "
        "switch (stage) { case Stage::Boot: break; default: break; } "
        "char marker = 'G'; "
        'mut std::string_view read_only_text = "gti"; '
        "read_only_text[0] = 'G'; "
        "EntityId entity_id = EntityId(1); "
        "Box<int> box = Box<int>(identity(1)); "
        "Box<int> direct_box{identity(1)}; "
        "StaticArray<int, 4> fixed = StaticArray<int, 4>(); "
        "uint64_t fixed_size = fixed.size(); "
        "std::array<int, 3> standard_array = std::array<int, 3>(); "
        "uint64_t standard_size = standard_array.size(); "
        "int& box_value = box.get(); "
        "int bits = ((identity(0b1) << 0x3) | 0x2) ^ 0b1; "
        "int remainder = bits % 3; int inverted = ~bits; "
        "auto inferred_count = identity(1); "
        'std::string_view invalid_constraint = constrained("text"); '
        "mut auto changing_count = inferred_count; changing_count += 1; "
        "changing_count *= 2; changing_count >>= 1; "
        "auto add_offset = [fixed_size](uint64_t value) -> uint64_t { "
        "return fixed_size + value; }; "
        "uint64_t lambda_value = add_offset(uint64_t(1)); "
        "mut Pixel pixel = Pixel(identity<int>(1)); pixel.reset(); "
        "mut Handle handle = Handle(); handle->reset(); *handle = 1; "
        "uint64_t invoked = handle(uint64_t(1)); "
        "handle[uint64_t(0)] += 1; bool present = handle != nullptr; "
        "int selected_value = present ? 1 : 2; "
        "if (handle && present || false) { *handle += 1; } "
        "mut std::unique_ptr<Pixel> owner = std::make_unique<Pixel>(1); "
        "std::unique_ptr<Pixel> moved = std::move(owner); "
        "auto copied_owner = moved; "
        "int borrowed = inspect_pixel(*moved); int moved_value = owner->x; "
        "uint64_t exact = overloaded(uint64_t(1)); overloaded(1); "
        'Shade invalid_shade = Shade("bad"); '
        "mut int buffer[1 + 2] = {1, 2, 3}; buffer[1] += 2; "
        "int invalid_array = buffer[3]; uint64_t buffer_size = buffer.size(); "
        "mut int iterations = 0; while (iterations < 2) { iterations++; "
        "if (iterations == 1) { continue; } break; } "
        "mut int post_test = 0; do { post_test++; } while (post_test < 1); "
        "[[discard]] identity(1); calculate(false); int hello = identity(1); "
        "hello = 2; int8_t small = 1; uint8_t byte = 255; return 0; } "
        "// entry point\n"
    )
    recovery_source = "".join(
        f"int stable_{index} = {index};\n" for index in range(2500)
    ) + (
        "int broken = 1\n"
        "int main() { break; int fixed = 1; fixed = 2; return 0; }\n"
    )
    requests = [
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"capabilities": {}},
        },
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
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
        },
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        },
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 4, "insertSpaces": True},
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": stress_uri,
                    "languageId": "gti",
                    "version": 1,
                    "text": "int main() { return 0; }\n",
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": stress_uri, "version": 2},
                "contentChanges": [{"text": recovery_source}],
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": stress_uri},
                "options": {"tabSize": 4, "insertSpaces": True},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": stress_uri}},
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didSave",
            "params": {"textDocument": {"uri": stress_uri}},
        },
        {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]

    process = subprocess.run(
        [sys.argv[1]],
        input=b"".join(frame(request) for request in requests),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=15,
    )
    if process.returncode != 0:
        raise AssertionError(
            f"gti_lsp exited with {process.returncode}: {process.stderr.decode()}"
        )

    messages = decode_messages(process.stdout)
    by_id = {message.get("id"): message for message in messages if "id" in message}
    initialization = by_id[1]["result"]["capabilities"]
    assert "semanticTokensProvider" in initialization
    assert initialization["documentFormattingProvider"] is True
    assert initialization["hoverProvider"] is True
    assert by_id[1]["result"]["serverInfo"]["version"] == expected_version

    legend = initialization["semanticTokensProvider"]["legend"]
    assert legend["tokenTypes"] == [
        "keyword",
        "type",
        "typeParameter",
        "namespace",
        "class",
        "function",
        "method",
        "variable",
        "parameter",
        "property",
        "string",
        "number",
        "operator",
        "macro",
        "decorator",
        "comment",
        "enumMember",
        "struct",
        "enum",
    ]
    assert legend["tokenModifiers"] == [
        "declaration",
        "definition",
        "readonly",
        "defaultLibrary",
        "functionScope",
        "static",
    ]

    publications = [
        message["params"]
        for message in messages
        if message.get("method") == "textDocument/publishDiagnostics"
    ]
    initial_publications = [
        params
        for params in publications
        if params["uri"] == uri and params.get("version") == 1
    ]
    assert len(initial_publications) == 1
    initial_publication = initial_publications[0]
    diagnostics = initial_publication["diagnostics"]
    assert len(diagnostics) == 11, diagnostics
    missing_return = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2031"
    )
    missing_return_start = source.index("incomplete")
    assert missing_return["range"] == {
        "start": lsp_position(source, missing_return_start),
        "end": lsp_position(
            source, missing_return_start + len("incomplete")
        ),
    }
    assert "can reach the end" in missing_return["message"]
    immutable = next(
        diagnostic
        for diagnostic in diagnostics
        if "immutable binding" in diagnostic["message"]
    )
    assert immutable["severity"] == 1
    assert immutable["source"] == "gti"
    assert immutable["code"] == "GTI-S2002"
    immutable_start = source.index("hello = 2")
    assert immutable["range"] == {
        "start": lsp_position(source, immutable_start),
        "end": lsp_position(source, immutable_start + len("hello")),
    }
    assert immutable["relatedInformation"][0]["location"]["uri"] == uri
    assert "Binding declared here" in immutable["relatedInformation"][0]["message"]
    receiver_mutation = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2002"
        and "read-only receiver" in diagnostic["message"]
    )
    receiver_field = source.index("slots[0]")
    assert receiver_mutation["range"] == {
        "start": lsp_position(source, receiver_field),
        "end": lsp_position(source, receiver_field + len("slots")),
    }
    assert "trailing 'mut'" in receiver_mutation["message"]
    assert "declared mutable here" in receiver_mutation["relatedInformation"][0][
        "message"
    ]
    assert any(
        "Function return value must be used" in diagnostic["message"]
        for diagnostic in diagnostics
    )
    overload = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2012"
        and "overloaded" in diagnostic["message"]
    )
    assert "exactly matches argument types (int32_t)" in overload["message"]
    assert len(overload["relatedInformation"]) == 2
    assert all(
        "Candidate:" in related["message"]
        for related in overload["relatedInformation"]
    )
    constructor_overload = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2012"
        and "constructor of 'Shade'" in diagnostic["message"]
    )
    assert "argument types (std::string_view)" in constructor_overload["message"]
    assert len(constructor_overload["relatedInformation"]) == 2
    assert all(
        "Candidate: Shade(" in related["message"]
        for related in constructor_overload["relatedInformation"]
    )
    array_bounds = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2016"
    )
    assert "valid range [0, 3)" in array_bounds["message"]
    invalid_index = source.index(
        "buffer[3]", source.index("invalid_array")
    ) + len("buffer[")
    assert array_bounds["range"] == {
        "start": lsp_position(source, invalid_index),
        "end": lsp_position(source, invalid_index + 1),
    }
    string_view_mutation = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2035"
        and "character access is read-only" in diagnostic["message"]
    )
    string_view_bracket = source.index("[", source.index("read_only_text[0]"))
    assert string_view_mutation["range"] == {
        "start": lsp_position(source, string_view_bracket),
        "end": lsp_position(source, string_view_bracket + 1),
    }
    assert not any(
        "missing_name" in diagnostic["message"] for diagnostic in diagnostics
    )
    moved_owner = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2018"
    )
    assert "has already been moved" in moved_owner["message"]
    moved_owner_start = source.index("owner->x")
    assert moved_owner["range"] == {
        "start": lsp_position(source, moved_owner_start),
        "end": lsp_position(source, moved_owner_start + len("owner")),
    }
    inferred_copy = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2003"
        and "inferred binding 'copied_owner'" in diagnostic["message"]
    )
    inferred_copy_start = source.index(
        "moved;", source.index("auto copied_owner")
    )
    assert inferred_copy["range"] == {
        "start": lsp_position(source, inferred_copy_start),
        "end": lsp_position(source, inferred_copy_start + len("moved")),
    }
    assert "std::move(owner)" in inferred_copy["message"]
    constraint_diagnostic = next(
        diagnostic
        for diagnostic in diagnostics
        if diagnostic["code"] == "GTI-S2029"
        and "std::numeric" in diagnostic["message"]
    )
    constraint_call = source.index("constrained(\"text\")")
    constraint_close = source.index(")", constraint_call)
    assert constraint_diagnostic["range"] == {
        "start": lsp_position(source, constraint_close),
        "end": lsp_position(source, constraint_close + 1),
    }
    constraint_name = source.index("numeric", source.index("std::numeric T"))
    assert constraint_diagnostic["relatedInformation"][0]["location"]["range"] == {
        "start": lsp_position(source, constraint_name),
        "end": lsp_position(source, constraint_name + len("numeric")),
    }

    library_uri = library_path.resolve().as_uri()
    dependency_publication = next(
        params
        for params in publications
        if params["uri"] == library_uri and params["diagnostics"]
    )
    dependency_diagnostic = dependency_publication["diagnostics"][0]
    assert dependency_diagnostic["code"] == "GTI-S2003"
    dependency_start = library_source.index('"bad"')
    assert dependency_diagnostic["range"] == {
        "start": lsp_position(library_source, dependency_start),
        "end": lsp_position(library_source, dependency_start + len('"bad"')),
    }

    recovered_publications = [
        params
        for params in publications
        if params["uri"] == stress_uri and params.get("version") == 2
    ]
    assert len(recovered_publications) == 1
    recovered_publication = recovered_publications[0]
    recovered_codes = {
        diagnostic["code"] for diagnostic in recovered_publication["diagnostics"]
    }
    assert {"GTI-P0001", "GTI-S2002", "GTI-S2010"}.issubset(recovered_codes)
    recovered_parse = next(
        diagnostic
        for diagnostic in recovered_publication["diagnostics"]
        if diagnostic["code"] == "GTI-P0001"
    )
    assert recovered_parse["data"]["fixes"][0]["replacement"] == ";"

    token_data = by_id[2]["result"]["data"]
    assert token_data and len(token_data) % 5 == 0
    assert token_data[3] == 13
    assert {2, 4, 8, 9, 12, 13, 14, 15, 16}.issubset(
        set(token_data[3::5])
    )
    assert any(modifier != 0 for modifier in token_data[4::5])

    token_types_by_position = {}
    token_modifiers_by_position = {}
    line = 0
    character = 0
    for index in range(0, len(token_data), 5):
        delta_line, delta_start, _, token_type, _ = token_data[index : index + 5]
        if delta_line:
            line += delta_line
            character = delta_start
        else:
            character += delta_start
        token_types_by_position[(line, character)] = token_type
        token_modifiers_by_position[(line, character)] = token_data[index + 4]
    for keyword in (
        "switch",
        "case",
        "default",
        "do",
        "continue",
        "break",
        "unsafe",
    ):
        suffix = {"do": " {", "continue": ";", "break": ";"}.get(keyword, "")
        position = lsp_position(source, source.index(keyword + suffix))
        assert token_types_by_position[(position["line"], position["character"])] == 0
    extern_keyword = lsp_position(source, source.index("extern"))
    assert token_types_by_position[
        (extern_keyword["line"], extern_keyword["character"])
    ] == 0
    const_keyword = lsp_position(source, source.index("const uint8_t*"))
    assert token_types_by_position[
        (const_keyword["line"], const_keyword["character"])
    ] == 0
    pointer_declarator = source.index("*", source.index("raw_write"))
    pointer_declarator_position = lsp_position(source, pointer_declarator)
    assert token_types_by_position[
        (
            pointer_declarator_position["line"],
            pointer_declarator_position["character"],
        )
    ] == 12
    address_of = lsp_position(source, source.index("&writable"))
    assert token_types_by_position[
        (address_of["line"], address_of["character"])
    ] == 12
    dereference = lsp_position(source, source.index("*alias ="))
    assert token_types_by_position[
        (dereference["line"], dereference["character"])
    ] == 12
    extern_language = lsp_position(source, source.index('"C"'))
    assert token_types_by_position[
        (extern_language["line"], extern_language["character"])
    ] == 10
    extern_function = lsp_position(source, source.index("native_close"))
    extern_function_key = (
        extern_function["line"],
        extern_function["character"],
    )
    assert token_types_by_position[extern_function_key] == 5
    assert token_modifiers_by_position[extern_function_key] & 1
    extern_parameter = lsp_position(source, source.index("descriptor"))
    extern_parameter_key = (
        extern_parameter["line"],
        extern_parameter["character"],
    )
    assert token_types_by_position[extern_parameter_key] == 8
    assert token_modifiers_by_position[extern_parameter_key] & 1
    error_directive = lsp_position(source, source.index("#error"))
    assert token_types_by_position[
        (error_directive["line"], error_directive["character"])
    ] == 13
    include_directive = lsp_position(source, source.index("#include"))
    assert token_types_by_position[
        (include_directive["line"], include_directive["character"])
    ] == 13
    syntax_owned_offsets = (
        source.index("this.value"),
        source.index("false", source.index("operator==(nullptr_t")),
        source.index("true", source.index("operator!=(nullptr_t")),
        source.index("nullptr", source.index("handle != nullptr")),
    )
    for offset in syntax_owned_offsets:
        position = lsp_position(source, offset)
        assert (
            position["line"],
            position["character"],
        ) not in token_types_by_position
    self_identifier = source.index("self", source.index("int self = 1"))
    self_position = lsp_position(source, self_identifier)
    self_token = token_types_by_position[
        (self_position["line"], self_position["character"])
    ]
    assert self_token == 7, self_token
    for spelling in ("&&", "||"):
        offset = source.index(spelling, source.index("if (handle"))
        position = lsp_position(source, offset)
        assert token_types_by_position[(position["line"], position["character"])] == 12
    compound_start = source.index("changing_count *= 2")
    for spelling in ("*=", ">>="):
        offset = source.index(spelling, compound_start)
        position = lsp_position(source, offset)
        assert token_types_by_position[(position["line"], position["character"])] == 12
    conditional_start = source.index("present ? 1 : 2")
    for spelling in ("?", ":"):
        offset = source.index(spelling, conditional_start)
        position = lsp_position(source, offset)
        assert token_types_by_position[(position["line"], position["character"])] == 12
    alias_name = source.index("EntityId", source.index("using EntityId"))
    alias_name_position = lsp_position(source, alias_name)
    assert token_types_by_position[
        (alias_name_position["line"], alias_name_position["character"])
    ] == 1
    assert token_modifiers_by_position[
        (alias_name_position["line"], alias_name_position["character"])
    ] & 3
    enum_type = source.index("Stage", source.index("enum class Stage"))
    enum_type_position = lsp_position(source, enum_type)
    enum_type_token = token_types_by_position[
        (enum_type_position["line"], enum_type_position["character"])
    ]
    semantic_identifiers = enum_type_token == 18
    assert enum_type_token in {1, 18}, enum_type_token
    enum_value = source.index("Boot", source.index("enum class Stage"))
    enum_value_position = lsp_position(source, enum_value)
    assert token_types_by_position[
        (enum_value_position["line"], enum_value_position["character"])
    ] == 16
    assert token_modifiers_by_position[
        (enum_value_position["line"], enum_value_position["character"])
    ] & 7 == 7
    enum_reference = source.index("Boot", source.index("Stage stage"))
    enum_reference_position = lsp_position(source, enum_reference)
    assert token_types_by_position[
        (enum_reference_position["line"], enum_reference_position["character"])
    ] == 16
    type_parameter = source.index("T", source.index("class Box<T>"))
    type_parameter_position = lsp_position(source, type_parameter)
    assert token_types_by_position[
        (type_parameter_position["line"], type_parameter_position["character"])
    ] == 2
    value_parameter = source.index("N", source.index("uint64_t N"))
    value_parameter_position = lsp_position(source, value_parameter)
    assert token_types_by_position[
        (value_parameter_position["line"], value_parameter_position["character"])
    ] == 8
    assert token_modifiers_by_position[
        (value_parameter_position["line"], value_parameter_position["character"])
    ] & 4
    constraint = source.index("std::numeric T")
    constraint_namespace_position = lsp_position(source, constraint)
    constraint_namespace_key = (
        constraint_namespace_position["line"],
        constraint_namespace_position["character"],
    )
    if not semantic_identifiers:
        assert constraint_namespace_key in token_types_by_position
    if constraint_namespace_key in token_types_by_position:
        assert token_types_by_position[constraint_namespace_key] == 3
        assert token_modifiers_by_position[constraint_namespace_key] & 8
    constraint_name_position = lsp_position(source, constraint + len("std::"))
    constraint_name_key = (
        constraint_name_position["line"],
        constraint_name_position["character"],
    )
    if not semantic_identifiers:
        assert constraint_name_key in token_types_by_position
    if constraint_name_key in token_types_by_position:
        assert token_types_by_position[constraint_name_key] == 1
        assert token_modifiers_by_position[constraint_name_key] & 8
    constrained_type_parameter_position = lsp_position(
        source, constraint + len("std::numeric ")
    )
    assert token_types_by_position[
        (
            constrained_type_parameter_position["line"],
            constrained_type_parameter_position["character"],
        )
    ] == 2
    direct_box_type = source.index("Box<int> direct_box")
    direct_box_type_position = lsp_position(source, direct_box_type)
    assert token_types_by_position[
        (direct_box_type_position["line"], direct_box_type_position["character"])
    ] == 4
    direct_box_name_position = lsp_position(
        source, direct_box_type + len("Box<int> ")
    )
    assert token_types_by_position[
        (direct_box_name_position["line"], direct_box_name_position["character"])
    ] == 7
    standard_include = source.index("std/array")
    standard_include_position = lsp_position(source, standard_include)
    assert token_types_by_position[
        (standard_include_position["line"], standard_include_position["character"])
    ] == 10
    assert token_modifiers_by_position[
        (standard_include_position["line"], standard_include_position["character"])
    ] & 8
    symbolic_extent = source.index("N]", source.index("T values[N]"))
    symbolic_extent_position = lsp_position(source, symbolic_extent)
    assert token_types_by_position[
        (symbolic_extent_position["line"], symbolic_extent_position["character"])
    ] == 8
    array_binding = source.index("buffer[3]")
    array_binding_position = lsp_position(source, array_binding)
    assert token_types_by_position[
        (array_binding_position["line"], array_binding_position["character"])
    ] == 7
    constructor_field = source.index("value(value)")
    constructor_field_position = lsp_position(source, constructor_field)
    assert token_types_by_position[
        (constructor_field_position["line"], constructor_field_position["character"])
    ] == 9
    destructor_name = source.index("Pixel", source.index("~Pixel"))
    destructor_name_position = lsp_position(source, destructor_name)
    assert token_types_by_position[
        (destructor_name_position["line"], destructor_name_position["character"])
    ] == 6
    unique_type = source.index("unique_ptr")
    unique_type_position = lsp_position(source, unique_type)
    assert token_types_by_position[
        (unique_type_position["line"], unique_type_position["character"])
    ] == (4 if semantic_identifiers else 1)
    make_unique = source.index("make_unique")
    make_unique_position = lsp_position(source, make_unique)
    assert token_types_by_position[
        (make_unique_position["line"], make_unique_position["character"])
    ] == 5
    assert token_modifiers_by_position[
        (make_unique_position["line"], make_unique_position["character"])
    ] & 8
    arrow = source.index("->", source.index("moved_value"))
    arrow_position = lsp_position(source, arrow)
    assert token_types_by_position[
        (arrow_position["line"], arrow_position["character"])
    ] == 12
    arrow_member_position = lsp_position(source, arrow + len("->"))
    assert token_types_by_position[
        (arrow_member_position["line"], arrow_member_position["character"])
    ] == 9
    operator_keyword = source.index("operator->")
    operator_keyword_position = lsp_position(source, operator_keyword)
    assert token_types_by_position[
        (operator_keyword_position["line"], operator_keyword_position["character"])
    ] == 0
    operator_symbol_position = lsp_position(source, operator_keyword + len("operator"))
    assert token_types_by_position[
        (operator_symbol_position["line"], operator_symbol_position["character"])
    ] == 12
    nullptr_type = source.index("nullptr_t")
    nullptr_type_position = lsp_position(source, nullptr_type)
    assert token_types_by_position[
        (nullptr_type_position["line"], nullptr_type_position["character"])
    ] == 1
    pack_type = source.index("Args...")
    pack_type_position = lsp_position(source, pack_type)
    assert token_types_by_position[
        (pack_type_position["line"], pack_type_position["character"])
    ] == 2
    pack_operator_position = lsp_position(source, pack_type + len("Args"))
    assert token_types_by_position[
        (pack_operator_position["line"], pack_operator_position["character"])
    ] == 12
    pack_expansion = source.index("values...", source.index("void relay"))
    pack_expansion_position = lsp_position(source, pack_expansion)
    assert token_types_by_position[
        (pack_expansion_position["line"], pack_expansion_position["character"])
    ] == 8
    inferred_auto = source.index("auto inferred_count")
    inferred_auto_position = lsp_position(source, inferred_auto)
    assert token_types_by_position[
        (inferred_auto_position["line"], inferred_auto_position["character"])
    ] == 1
    inferred_binding = inferred_auto + len("auto ")
    inferred_binding_position = lsp_position(source, inferred_binding)
    assert token_types_by_position[
        (inferred_binding_position["line"], inferred_binding_position["character"])
    ] == 7
    assert token_modifiers_by_position[
        (inferred_binding_position["line"], inferred_binding_position["character"])
    ] & 5 == 5
    mutable_auto = source.index("auto changing_count")
    mutable_binding = mutable_auto + len("auto ")
    mutable_binding_position = lsp_position(source, mutable_binding)
    assert token_types_by_position[
        (mutable_binding_position["line"], mutable_binding_position["character"])
    ] == 7
    assert token_modifiers_by_position[
        (mutable_binding_position["line"], mutable_binding_position["character"])
    ] & 1
    assert not (
        token_modifiers_by_position[
            (mutable_binding_position["line"], mutable_binding_position["character"])
        ]
        & 4
    )
    auto_type = source.index("auto add_offset")
    auto_type_position = lsp_position(source, auto_type)
    assert token_types_by_position[
        (auto_type_position["line"], auto_type_position["character"])
    ] == 1
    lambda_capture = source.index("fixed_size]", source.index("auto add_offset"))
    lambda_capture_position = lsp_position(source, lambda_capture)
    assert token_types_by_position[
        (lambda_capture_position["line"], lambda_capture_position["character"])
    ] == 7
    assert token_modifiers_by_position[
        (lambda_capture_position["line"], lambda_capture_position["character"])
    ] & 4
    character_type = source.index("char marker")
    character_type_position = lsp_position(source, character_type)
    assert token_types_by_position[
        (character_type_position["line"], character_type_position["character"])
    ] == 1
    character_literal = source.index("'G'")
    character_literal_position = lsp_position(source, character_literal)
    assert token_types_by_position[
        (
            character_literal_position["line"],
            character_literal_position["character"],
        )
    ] == 10

    formatting_edits = by_id[3]["result"]
    assert len(formatting_edits) == 1
    formatted = formatting_edits[0]["newText"]
    assert "namespace engine {\n    namespace graphics" in formatted
    assert "class Box<T> {" in formatted
    assert "T & get() {" in formatted
    assert "Box<int> box = Box<int>(identity(1));" in formatted
    assert "class StaticArray<T, uint64_t N> {" in formatted
    assert "// Generic iterator access.\n    Box<T> begin();" in formatted
    assert "#include <std/array>" in formatted
    assert "using EntityId = uint64_t;" in formatted
    assert (
        "    switch (stage) {\n"
        "    case Stage::Boot:\n"
        "        break;\n"
        "    default:\n"
        "        break;\n"
        "    }" in formatted
    )
    assert "char marker = 'G';" in formatted
    assert (
        "void raw_write(uint8_t* writable, const uint8_t* readable) {"
        in formatted
    )
    assert "uint8_t* alias = &writable[0];" in formatted
    assert "*alias = readable[0];" in formatted
    assert "T values[N] = {};" in formatted
    assert "StaticArray<int, 4> fixed = StaticArray<int, 4>();" in formatted
    assert "Box<int> direct_box{identity(1)};" in formatted
    assert "std::array<int, 3> standard_array = std::array<int, 3>();" in formatted
    assert "int & box_value = box.get();" in formatted
    assert "identity<int>(1)" in formatted
    assert "T constrained<std::numeric T>(T value) {" in formatted
    assert "int bits = ((identity(0b1) << 0x3) | 0x2) ^ 0b1;" in formatted
    assert "int remainder = bits % 3;" in formatted
    assert "int inverted = ~bits;" in formatted
    assert "auto inferred_count = identity(1);" in formatted
    assert 'std::string_view invalid_constraint = constrained("text");' in formatted
    assert "mut auto changing_count = inferred_count;" in formatted
    assert "changing_count += 1;" in formatted
    assert (
        "auto add_offset = [fixed_size](uint64_t value) -> uint64_t {" in formatted
    )
    assert "while (iterations < 2) {\n        iterations++;" in formatted
    assert "            continue;\n        }\n        break;" in formatted
    assert "expected<int, int> calculate(bool fail) {" in formatted
    assert "uint64_t exact = overloaded(uint64_t(1));" in formatted
    assert "mut int buffer[1 + 2] = {1, 2, 3};" in formatted
    assert "int inspect_pixel(Pixel & pixel) {" in formatted
    assert "mut std::unique_ptr<Pixel> owner = std::make_unique<Pixel>(1);" in formatted
    assert "auto copied_owner = moved;" in formatted
    assert "int moved_value = owner->x;" in formatted
    assert "int invalid_array = buffer[3];" in formatted
    assert "uint64_t buffer_size = buffer.size();" in formatted
    assert "struct Pixel {\npublic:\n    mut int x;\n    Pixel(int x) : x(x) {}" in formatted
    assert "void reset() mut {\n        this.x = 0;\n    }" in formatted
    assert "~Pixel() {\n        this.reset();\n    }\nprivate:" in formatted
    assert "mut Pixel & operator->() mut {" in formatted
    assert "mut int & operator*() mut {" in formatted
    assert "mut int & operator[](uint64_t index) mut {" in formatted
    assert "uint64_t operator()(uint64_t value) {" in formatted
    assert "operator bool() {" in formatted
    assert "void relay<Args...>(Args... values) {" in formatted
    assert "consume(values...);" in formatted
    assert "handle[uint64_t(0)] += 1;" in formatted
    assert "uint64_t invoked = handle(uint64_t(1));" in formatted
    assert "        return unexpected(1);" in formatted
    assert "std::print(" in formatted
    assert "int8_t small = 1;" in formatted
    assert "uint8_t byte = 255;" in formatted
    assert formatted.endswith("// entry point\n")
    responsive_format_index = next(
        index for index, message in enumerate(messages) if message.get("id") == 5
    )
    recovered_diagnostics_index = next(
        index
        for index, message in enumerate(messages)
        if message.get("method") == "textDocument/publishDiagnostics"
        and message["params"]["uri"] == stress_uri
        and message["params"].get("version") == 2
    )
    assert responsive_format_index < recovered_diagnostics_index
    assert by_id[6]["result"]["data"]
    assert by_id[4]["result"] is None

    test_unsaved_dependency_reanalysis(sys.argv[1], root)
    test_direct_dependency_visibility(sys.argv[1], root)
    test_missing_include_and_format_config(sys.argv[1], root)
    test_inheritance_tooling(sys.argv[1], root)


if __name__ == "__main__":
    main()
