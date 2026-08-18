#!/usr/bin/env python3

"""Protocol smoke test for the GTI language server."""

import json
import os
import pathlib
import select
import shutil
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


def discover_standard_library_root(executable):
    configured = os.environ.get("GTI_STDLIB_PATH")
    if configured:
        configured_path = pathlib.Path(configured).resolve()
        return (
            configured_path.parent
            if configured_path.suffix == ".gti"
            else configured_path
        )

    resolved_executable = shutil.which(executable)
    if resolved_executable:
        installed_root = (
            pathlib.Path(resolved_executable).resolve().parent.parent
            / "share"
            / "gti"
            / "stdlib"
        )
        if (installed_root / "prelude.gti").is_file():
            return installed_root

    return pathlib.Path(__file__).resolve().parent.parent / "stdlib"


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
        "interface Renderable{int render(int frame);};\n"
        "class Base{public:virtual int tick(int frame){return frame;}};\n"
        "class Sprite:public Base,public Renderable{public:"
        "int tick(int frame)override{return frame;}"
        "int render(int frame)override{return this.tick(frame);}};\n"
        "int invoke(Renderable& value){return value.render(1);}\n"
        "interface RangeIteratorContract<T>{T& operator*();"
        "void operator++()mut;};\n"
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
        assert "for (auto& value : values) {" in formatted

        invalid_source = (
            "interface Invalid { int state = 0; "
            "int legacy() = 0; };\n"
        )
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
        assert any(
            "implicitly pure" in item.get("message", "") for item in diagnostics
        )
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
                "params": {
                    "capabilities": {
                        "textDocument": {
                            "publishDiagnostics": {"relatedInformation": True}
                        }
                    }
                },
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
        "return(bits);}}return bits;}"
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
        assert "return (bits);" in formatted
    finally:
        session.close()


def test_semantic_hover(executable, root):
    source = (
        "uint64_t choose(uint64_t value) { return value; }\n"
        "float choose(float value) { return value; }\n"
        "interface Renderable { int render(); };\n"
        "[[no_transfer, unsafe_share]] class Affine { int value = 0; };\n"
        "void relay<Args...>(Args... values) {}\n"
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

        interface_offset = source.index("Renderable")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, interface_offset + 1),
                },
            }
        )
        interface_hover = session.receive_until(
            lambda message: message.get("id") == 5
        )["result"]
        assert interface_hover["contents"]["value"].startswith(
            "```gti\ninterface Renderable\n```"
        )

        variadic_offset = source.index("relay<Args")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, variadic_offset + 1),
                },
            }
        )
        variadic_hover = session.receive_until(
            lambda message: message.get("id") == 6
        )["result"]
        assert variadic_hover["contents"]["value"].startswith(
            "```gti\nvoid relay<Args...>(Args... values)\n```"
        )

        capability_offset = source.index("Affine")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, capability_offset + 1),
                },
            }
        )
        capability_hover = session.receive_until(
            lambda message: message.get("id") == 7
        )["result"]
        assert capability_hover["contents"]["value"].startswith(
            "```gti\nclass Affine\n```"
        )
        assert "*not transfer-capable (explicit opt-out)*" in capability_hover[
            "contents"
        ]["value"]
        assert "*share-capable (unsafe nominal assertion)*" in capability_hover[
            "contents"
        ]["value"]

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        capability_tokens = semantic_tokens_by_position(
            session.receive_until(lambda message: message.get("id") == 8)[
                "result"
            ]["data"]
        )
        for attribute in ("no_transfer", "unsafe_share"):
            position = lsp_position(source, source.index(attribute))
            assert capability_tokens[(position["line"], position["character"])][
                "type"
            ] == 14
    finally:
        session.close()


def test_callable_contract_hover(executable, root):
    source = (
        "T map<T, Operation>(T value, Operation operation) {\n"
        "  T result = operation(value);\n"
        "  return operation(result);\n"
        "}\n"
        "int apply_mut<Operation>(int value, mut Operation operation) {\n"
        "  return operation(value);\n"
        "}\n"
        "int dispatch<Operation>(int value, bool flag, Operation operation) {\n"
        "  if (flag) {\n"
        "    return operation(value);\n"
        "  }\n"
        "  return operation(flag);\n"
        "}\n"
        "class MemberMapper<T> {\n"
        "public:\n"
        "  T apply<Operation>(T value, Operation operation) {\n"
        "    return operation(value);\n"
        "  }\n"
        "};\n"
        "class CallableOwner<T> {\n"
        "  T value;\n"
        "public:\n"
        "  CallableOwner(T value) : value(std::move(value)) {}\n"
        "};\n"
        "T relay<T>(T value) { return std::move(value); }\n"
        "CallableOwner<T> own<T>(T value) {\n"
        "  return CallableOwner<T>(std::move(value));\n"
        "}\n"
        "int main() {\n"
        "  auto increment = [](int value) -> int { return value + 1; };\n"
        "  auto owned_operation = []() -> int { return 7; };\n"
        "  auto returned = relay(std::move(owned_operation));\n"
        "  auto retained = own(std::move(returned));\n"
        "  MemberMapper<int> mapper = MemberMapper<int>();\n"
        "  int member_result = mapper.apply(1, increment);\n"
        "  return map(1, increment) + member_result - 4;\n"
        "}\n"
    )
    path = root / "callable-contract-hover.gti"
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

        def hover(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]["contents"]["value"]

        declared_map = source.index("map<T, Operation>")
        declaration_hover = hover(2, declared_map + 1)
        assert (
            "*confined callable parameter 'operation' (read-only access), "
            "exact signature: read-callable (T) -> T*"
            in declaration_hover
        )

        selected_map = source.index("map(1")
        selected_hover = hover(3, selected_map + 1)
        assert (
            "*confined callable parameter 'operation' (read-only access), "
            "exact signature: read-callable (int32_t) -> int32_t*"
            in selected_hover
        )
        assert "exact signature: read-callable (T) -> T" not in selected_hover

        declared_member = source.index("apply<Operation>")
        declared_member_hover = hover(4, declared_member + 1)
        assert (
            "*confined callable parameter 'operation' (read-only access), "
            "exact signature: read-callable (T) -> T*"
            in declared_member_hover
        )

        selected_member = source.index("mapper.apply") + len("mapper.")
        selected_member_hover = hover(5, selected_member + 1)
        assert (
            "*confined callable parameter 'operation' (read-only access), "
            "exact signature: read-callable (int32_t) -> int32_t*"
            in selected_member_hover
        )
        assert (
            "exact signature: read-callable (T) -> T"
            not in selected_member_hover
        )

        mutable_apply = source.index("apply_mut<Operation>")
        mutable_hover = hover(6, mutable_apply + 1)
        assert (
            "*confined callable parameter 'operation' (mutable access), "
            "exact signature: mut-callable (int32_t) -> int32_t*"
            in mutable_hover
        )

        dispatch = source.index("dispatch<Operation>")
        dispatch_hover = hover(7, dispatch + 1)
        assert (
            "*confined callable parameter 'operation' (read-only access), "
            "exact signatures: read-callable (int32_t) -> int32_t; "
            "read-callable (bool) -> int32_t*"
            in dispatch_hover
        )

        relay = source.index("relay<T>")
        relay_hover = hover(8, relay + 1)
        assert (
            "*owned callable parameter 'value' (explicit ownership move), "
            "exact transport: return T*"
            in relay_hover
        )

        own = source.index("own<T>")
        own_hover = hover(9, own + 1)
        assert (
            "*owned callable parameter 'value' (explicit ownership move), "
            "exact transport: field of CallableOwner<T>*"
            in own_hover
        )
    finally:
        session.close()


def test_requires_tooling(executable, root):
    source = (
        "concept pairwise<Left, Right> =\n"
        "    std::numeric<Left> && std::numeric<Right>;\n"
        "Left choose<std::numeric Left, std::numeric Right>(\n"
        "    Left left, Right right)\n"
        "  requires pairwise<Left, Right> {\n"
        "  return left;\n"
        "}\n"
        "class NumericBox<T> {\n"
        "public:\n"
        "  T doubled(T value) requires std::numeric<T> {\n"
        "    return T(value + value);\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  NumericBox<int> box = NumericBox<int>();\n"
        "  int doubled = box.doubled(2);\n"
        "  return choose(doubled, 2);\n"
        "}\n"
    )
    path = root / "requires-tooling.gti"
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
                            "hover": {"contentFormat": ["markdown"]}
                        }
                    }
                },
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

        def hover(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        concept_declaration = source.index("pairwise")
        concept_use = source.index("pairwise", concept_declaration + 1)
        concept_hover = hover(2, concept_use + 1)
        assert concept_hover["contents"]["value"].startswith(
            "```gti\nconcept pairwise<Left, Right> = "
            "std::numeric<Left> && std::numeric<Right>\n```"
        )

        function_declaration = source.index("choose")
        function_hover = hover(3, function_declaration + 1)
        assert function_hover["contents"]["value"].startswith(
            "```gti\n"
            "Left choose<std::numeric Left, std::numeric Right>("
            "Left left, Right right) requires pairwise<Left, Right>\n```"
        )

        selected_call = source.rindex("choose")
        selected_hover = hover(4, selected_call + 1)
        assert selected_hover["contents"]["value"].startswith(
            "```gti\n"
            "int32_t choose<int32_t, int32_t>(int32_t left, int32_t right) "
            "requires pairwise<int32_t, int32_t>\n```"
        )

        selected_method = source.index("box.doubled") + len("box.")
        selected_method_hover = hover(5, selected_method + 1)
        assert selected_method_hover["contents"]["value"].startswith(
            "```gti\n"
            "int32_t NumericBox::doubled(int32_t value) "
            "requires std::numeric<int32_t>\n```"
        )

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, concept_use + 1),
                },
            }
        )
        definition = session.receive_until(lambda message: message.get("id") == 6)[
            "result"
        ]
        assert definition == {
            "uri": uri,
            "range": {
                "start": lsp_position(source, concept_declaration),
                "end": lsp_position(
                    source, concept_declaration + len("pairwise")
                ),
            },
        }

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        data = session.receive_until(lambda message: message.get("id") == 7)[
            "result"
        ]["data"]
        tokens = semantic_tokens_by_position(data)

        def token_type_at(offset):
            position = lsp_position(source, offset)
            return tokens[(position["line"], position["character"])]["type"]

        requires = source.index("requires")
        assert token_type_at(requires) == 0
        assert token_type_at(concept_use) == 1
        left_argument = source.index("Left", concept_use)
        right_argument = source.index("Right", left_argument + 1)
        assert token_type_at(left_argument) == 2
        assert token_type_at(right_argument) == 2

        member_completion_source = source.replace("box.doubled(2)", "box.dou")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": member_completion_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )
        member_prefix = member_completion_source.index("box.dou") + len("box.dou")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(
                        member_completion_source, member_prefix
                    ),
                },
            }
        )
        member_completion = session.receive_until(
            lambda message: message.get("id") == 8
        )["result"]
        doubled = next(
            item
            for item in member_completion["items"]
            if item["label"] == "doubled"
        )
        assert doubled["detail"] == (
            "int32_t NumericBox::doubled(int32_t value) "
            "requires std::numeric<int32_t>"
        )

        completion_source = source.replace(
            "return choose(doubled, 2);", "return cho;"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": completion_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
        )
        prefix = completion_source.rindex("cho")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(completion_source, prefix + 3),
                },
            }
        )
        completion = session.receive_until(lambda message: message.get("id") == 9)[
            "result"
        ]
        choose = next(item for item in completion["items"] if item["label"] == "choose")
        assert choose["detail"] == (
            "Left choose<std::numeric Left, std::numeric Right>("
            "Left left, Right right) requires pairwise<Left, Right>"
        )
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


def test_compiler_private_tooling_boundary(executable, root):
    source = (
        "namespace hidden = gti_internal;\n"
        "int main() {\n"
        "  int value = 1;\n"
        "  int moved = std::move(value);\n"
        "  int public_completion_probe = std::mo;\n"
        "  int direct_probe = gti_internal::allocate_storage;\n"
        "  int alias_probe = hidden::allocate_storage;\n"
        "  int private_completion_probe = gti_internal::sto;\n"
        "  return moved;\n"
        "}\n"
    )
    path = root / "compiler-private-tooling.gti"
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
                            "publishDiagnostics": {"dataSupport": True},
                            "completion": {
                                "completionItem": {"snippetSupport": True}
                            },
                            "hover": {
                                "contentFormat": ["markdown", "plaintext"]
                            },
                        }
                    }
                },
            }
        )
        capabilities = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]["capabilities"]
        assert capabilities["completionProvider"]
        assert capabilities["definitionProvider"] is True
        assert capabilities["hoverProvider"] is True
        assert capabilities["semanticTokensProvider"]

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
        )["params"]
        private_diagnostics = [
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2058"
        ]
        private_diagnostics_by_start = {
            (
                diagnostic["range"]["start"]["line"],
                diagnostic["range"]["start"]["character"],
            ): diagnostic
            for diagnostic in private_diagnostics
        }
        alias_target = source.index("gti_internal")
        direct_root = source.index("gti_internal", alias_target + 1)
        alias_use = source.index("hidden::")
        private_completion_root = source.index(
            "gti_internal", direct_root + len("gti_internal")
        )
        for offset, private_name in (
            (alias_target, "gti_internal"),
            (direct_root, "gti_internal::allocate_storage"),
            (alias_use, "hidden::allocate_storage"),
            (private_completion_root, "gti_internal::sto"),
        ):
            position = lsp_position(source, offset)
            diagnostic = private_diagnostics_by_start[
                (position["line"], position["character"])
            ]
            assert diagnostic["message"].splitlines()[0] == (
                f"Compiler-private name '{private_name}' is unavailable to "
                "application source."
            ), diagnostic

        direct_leaf = source.index("allocate_storage", direct_root)
        alias_leaf = source.index("allocate_storage", direct_leaf + 1)

        def semantic_request(request_id, method, offset, request_uri=uri,
                             request_source=source):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": method,
                    "params": {
                        "textDocument": {"uri": request_uri},
                        "position": lsp_position(request_source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        assert semantic_request(2, "textDocument/hover", direct_leaf + 1) is None
        assert (
            semantic_request(3, "textDocument/definition", direct_leaf + 1)
            is None
        )
        assert semantic_request(4, "textDocument/hover", alias_leaf + 1) is None
        assert (
            semantic_request(5, "textDocument/definition", alias_leaf + 1)
            is None
        )

        private_prefix = source.index("sto;", private_completion_root)
        private_completion = semantic_request(
            6,
            "textDocument/completion",
            private_prefix + len("sto"),
        )
        assert private_completion["items"] == []

        public_prefix = source.index("mo;", source.index("public_completion_probe"))
        public_completion = semantic_request(
            7,
            "textDocument/completion",
            public_prefix + len("mo"),
        )
        move_item = next(
            item for item in public_completion["items"] if item["label"] == "move"
        )
        assert "std::move" in move_item["detail"]

        public_move = source.index("move(value)")
        public_hover = semantic_request(8, "textDocument/hover", public_move + 1)
        assert "std::move" in public_hover["contents"]["value"]
        public_definition = semantic_request(
            9, "textDocument/definition", public_move + 1
        )
        assert public_definition is not None
        assert public_definition["uri"] != uri

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        semantic_data = session.receive_until(
            lambda message: message.get("id") == 10
        )["result"]["data"]
        semantic_tokens = semantic_tokens_by_position(semantic_data)
        for offset in (
            alias_target,
            direct_root,
            direct_leaf,
            alias_use,
            alias_leaf,
            private_completion_root,
            private_prefix,
        ):
            position = lsp_position(source, offset)
            assert (
                position["line"],
                position["character"],
            ) not in semantic_tokens

        public_move_position = lsp_position(source, public_move)
        public_move_token = semantic_tokens[
            (public_move_position["line"], public_move_position["character"])
        ]
        assert public_move_token["type"] == 5
        assert public_move_token["modifiers"] & 8

        stdlib_path = (
            discover_standard_library_root(executable) / "std" / "vector.gti"
        )
        if stdlib_path.is_file():
            stdlib_source = stdlib_path.read_text(encoding="utf-8")
            stdlib_uri = stdlib_path.resolve().as_uri()
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": stdlib_uri,
                            "languageId": "gti",
                            "version": 1,
                            "text": stdlib_source,
                        }
                    },
                }
            )
            stdlib_publication = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == stdlib_uri
                and message["params"].get("version") == 1
            )["params"]
            assert not any(
                diagnostic.get("code") == "GTI-S2058"
                for diagnostic in stdlib_publication["diagnostics"]
            )
            private_call = stdlib_source.index("storage_read")
            trusted_hover = semantic_request(
                11,
                "textDocument/hover",
                private_call + 1,
                stdlib_uri,
                stdlib_source,
            )
            assert "gti_internal::prefix_storage_read" in trusted_hover[
                "contents"
            ]["value"]
            trusted_definition = semantic_request(
                12,
                "textDocument/definition",
                private_call + 1,
                stdlib_uri,
                stdlib_source,
            )
            assert trusted_definition is not None
            assert trusted_definition["uri"] != stdlib_uri
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
                            "publishDiagnostics": {"dataSupport": True},
                            "codeAction": {
                                "isPreferredSupport": True,
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
        assert diagnostic["data"]["phase"] == "parsing"
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
                "id": 3,
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 0, "character": 1},
                    },
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
                "id": 4,
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
        assert session.receive_until(lambda message: message.get("id") == 4)[
            "result"
        ] == []
    finally:
        session.close()


def test_diagnostic_capability_negotiation(executable, root):
    source = (
        "int duplicate = 1;\n"
        "int duplicate = 2;\n"
        'int main() { string text = "value"; return 0; }\n'
    )
    path = root / "diagnostic-capabilities.gti"
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
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        assert "codeActionProvider" not in initialization["capabilities"]
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
            and bool(message["params"]["diagnostics"])
        )["params"]
        duplicate = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2006"
        )
        legacy_string = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2033"
        )
        assert "relatedInformation" not in duplicate
        assert "data" not in legacy_string
        assert "help: An owning std::string type is not available yet." in legacy_string[
            "message"
        ]
    finally:
        session.close()


def test_current_language_diagnostics(executable, root):
    source = (
        "class Cleanup { public: Cleanup() {} ~Cleanup() {} };\n"
        "Cleanup global_cleanup{};\n"
        "int32_t runtime_value() { return 1; }\n"
        "constexpr int32_t called = runtime_value();\n"
        'int main(int bad) { string text = "value"; return bad; }\n'
    )
    path = root / "current-language-diagnostics.gti"
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
                            "publishDiagnostics": {
                                "relatedInformation": True,
                                "dataSupport": True,
                            },
                            "codeAction": {
                                "codeActionLiteralSupport": {
                                    "codeActionKind": {
                                        "valueSet": ["quickfix"]
                                    }
                                }
                            },
                        }
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
            and bool(message["params"]["diagnostics"])
        )["params"]
        constexpr_diagnostic = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2057"
        )
        assert constexpr_diagnostic["data"]["phase"] == "semantics"
        assert constexpr_diagnostic["data"]["hints"]
        assert constexpr_diagnostic["relatedInformation"][0]["location"][
            "uri"
        ] == uri
        constexpr_start = source.index("constexpr")
        assert constexpr_diagnostic["relatedInformation"][0]["location"][
            "range"
        ]["start"] == lsp_position(source, constexpr_start)

        legacy_string = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2033"
        )
        assert legacy_string["data"]["phase"] == "semantics"
        assert legacy_string["data"]["fixes"]

        entry_diagnostic = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2032"
        )
        assert entry_diagnostic["data"]["phase"] == "semantics"
        assert "std::vector<std::string>" in entry_diagnostic["message"]
        assert entry_diagnostic["data"]["hints"]
        parameter_start = source.index("int bad")
        assert entry_diagnostic["range"] == {
            "start": lsp_position(source, parameter_start),
            "end": lsp_position(source, parameter_start + 3),
        }

        lifecycle = next(
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2061"
        )
        lifecycle_name = source.index("global_cleanup")
        assert lifecycle["range"] == {
            "start": lsp_position(source, lifecycle_name),
            "end": lsp_position(
                source, lifecycle_name + len("global_cleanup")
            ),
        }, lifecycle
        assert lifecycle["data"]["phase"] == "semantics", lifecycle
        assert lifecycle["data"]["hints"], lifecycle
        assert lifecycle["relatedInformation"], lifecycle
        destructor = source.index("~Cleanup")
        assert lifecycle["relatedInformation"][0]["location"]["range"] == {
            "start": lsp_position(source, destructor),
            "end": lsp_position(source, destructor + 1),
        }, lifecycle
        assert "fixes" not in lifecycle["data"], lifecycle
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": legacy_string["range"],
                    "context": {
                        "diagnostics": [legacy_string],
                        "only": ["quickfix"],
                    },
                },
            }
        )
        actions = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert len(actions) == 1, actions
        assert "isPreferred" not in actions[0]
    finally:
        session.close()


def test_global_borrow_return_diagnostics(executable, root):
    valid_source = (
        "class Application { public: "
        "static mut Application& Get() { "
        "unsafe { return *Application::app; } } "
        "void Update() mut {} private: "
        "static mut Application* app = nullptr; };\n"
        "void update() { Application::Get().Update(); }\n"
        "int main() { update(); return 0; }\n"
    )
    invalid_source = (
        "class Application { public: "
        "static mut Application& Get() { "
        "unsafe { return *Application::app; } } private: "
        "static mut Application* app = nullptr; };\n"
        "int main() {\n"
        "  mut Application& first = Application::Get();\n"
        "  mut Application& second = Application::Get();\n"
        "  return 0;\n"
        "}\n"
    )
    path = root / "global-borrow-return.gti"
    path.write_text(valid_source, encoding="utf-8")
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
                            "publishDiagnostics": {
                                "relatedInformation": True,
                                "dataSupport": True,
                            }
                        }
                    }
                },
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
                        "text": valid_source,
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
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": invalid_source}],
                },
            }
        )
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]
        conflicts = [
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2017"
            and "Cannot create a mutable borrow" in diagnostic.get("message", "")
        ]
        assert len(conflicts) == 1, publication
        conflict = conflicts[0]
        second_call = invalid_source.rindex("Application::Get()") + len(
            "Application::Get("
        )
        assert conflict["range"]["start"] == lsp_position(
            invalid_source, second_call
        ), conflict
        assert conflict["data"]["phase"] == "semantics", conflict
        assert conflict["data"]["hints"], conflict
        assert conflict["relatedInformation"], conflict
        assert "Retained borrow originates here" in conflict[
            "relatedInformation"
        ][0]["message"]

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": valid_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
            and not message["params"]["diagnostics"]
        )
    finally:
        session.close()


def test_unique_ptr_null_state_tooling(executable, root):
    valid_source = (
        "class Example { public: Example() {} };\n"
        "void consume(std::unique_ptr<Example> value) {}\n"
        "int main() {\n"
        "  mut std::unique_ptr<Example> value = nullptr;\n"
        "  value.reset();\n"
        "  value = nullptr;\n"
        "  return 0;\n"
        "}\n"
    )
    invalid_source = valid_source.replace(
        "  return 0;\n", "  consume(nullptr);\n  return 0;\n"
    )
    path = root / "unique-ptr-null-state.gti"
    path.write_text(valid_source, encoding="utf-8")
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
                            "publishDiagnostics": {
                                "relatedInformation": True,
                                "dataSupport": True,
                            }
                        }
                    }
                },
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
                        "text": valid_source,
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
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": invalid_source}],
                },
            }
        )
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]
        mismatches = [
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2003"
            and "has type 'nullptr_t'" in diagnostic.get("message", "")
        ]
        assert len(mismatches) == 1, publication
        mismatch = mismatches[0]
        null_argument = invalid_source.rindex("nullptr")
        assert mismatch["range"] == {
            "start": lsp_position(invalid_source, null_argument),
            "end": lsp_position(invalid_source, null_argument + len("nullptr")),
        }, mismatch
        assert mismatch["data"]["phase"] == "semantics", mismatch
        assert mismatch["data"]["hints"], mismatch

        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": valid_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
            and not message["params"]["diagnostics"]
        )
    finally:
        session.close()


def test_cpp_reserved_identifier_diagnostic(executable, root):
    source = "int main() { int template = 1; return 0; }\n"
    path = root / "reserved-identifier.gti"
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
                            "publishDiagnostics": {"dataSupport": True}
                        }
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        token_types = initialization["capabilities"]["semanticTokensProvider"][
            "legend"
        ]["tokenTypes"]
        keyword_type = token_types.index("keyword")
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
            and bool(message["params"]["diagnostics"])
        )["params"]
        assert len(publication["diagnostics"]) == 1, publication
        diagnostic = publication["diagnostics"][0]
        start = source.index("template")
        assert diagnostic["code"] == "GTI-P0002", diagnostic
        assert diagnostic["data"] == {"phase": "parsing"}, diagnostic
        assert diagnostic["message"] == (
            "'template' is a reserved C++ keyword and cannot be used as a GTI "
            "identifier."
        )
        assert diagnostic["range"] == {
            "start": lsp_position(source, start),
            "end": lsp_position(source, start + len("template")),
        }

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        token_data = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]["data"]
        tokens = semantic_tokens_by_position(token_data)
        position = lsp_position(source, start)
        token = tokens[(position["line"], position["character"])]
        assert token["type"] == keyword_type, token
        assert token["length"] == len("template"), token
    finally:
        session.close()


def test_contextual_signed_integer_diagnostic(executable, root):
    source = (
        "int main() {\n"
        "  uint8_t value = 5;\n"
        "  bool invalid = value == -1;\n"
        "  return 0;\n"
        "}\n"
    )
    path = root / "contextual-signed-integer.gti"
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
                            "publishDiagnostics": {"dataSupport": True}
                        }
                    }
                },
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
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
        )["params"]
        diagnostics = [
            diagnostic
            for diagnostic in publication["diagnostics"]
            if diagnostic.get("code") == "GTI-S2004"
        ]
        assert len(diagnostics) == 1, publication
        diagnostic = diagnostics[0]
        digit = source.rindex("-1") + 1
        assert "Integer literal '-1'" in diagnostic["message"], diagnostic
        assert diagnostic["data"]["phase"] == "semantics", diagnostic
        assert len(diagnostic["data"]["hints"]) == 1, diagnostic
        assert "fixes" not in diagnostic["data"], diagnostic
        assert diagnostic["range"] == {
            "start": lsp_position(source, digit),
            "end": lsp_position(source, digit + 1),
        }, diagnostic
    finally:
        session.close()


def test_layout_query_tooling(executable, root):
    source = (
        "using Word = uint32_t;\n"
        "constexpr double precise = 0.1d;\n"
        "constexpr uint64_t word_size = sizeof (Word);\n"
        "constexpr uint64_t word_alignment = alignof(Word);\n"
        "int main() { return int32_t(word_size - word_alignment); }\n"
    )
    path = root / "layout-query-tooling.gti"
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
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        token_types = initialization["capabilities"]["semanticTokensProvider"][
            "legend"
        ]["tokenTypes"]
        operator_type = token_types.index("operator")
        builtin_type = token_types.index("type")
        number_type = token_types.index("number")
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
        assert publication["params"]["diagnostics"] == [], publication

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        token_data = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]["data"]
        tokens = semantic_tokens_by_position(token_data)
        double_position = lsp_position(source, source.index("double"))
        double_token = tokens[(double_position["line"],
                               double_position["character"])]
        assert double_token["type"] == builtin_type, double_token
        literal_position = lsp_position(source, source.index("0.1d"))
        literal_token = tokens[(literal_position["line"],
                                literal_position["character"])]
        assert literal_token["type"] == number_type, literal_token
        assert literal_token["length"] == len("0.1d"), literal_token
        for spelling in ("sizeof", "alignof"):
            position = lsp_position(source, source.index(spelling))
            token = tokens[(position["line"], position["character"])]
            assert token["type"] == operator_type, (spelling, token)
            assert token["length"] == len(spelling), (spelling, token)

        sizeof_offset = source.index("sizeof")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, sizeof_offset + 1),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ]
        assert hover and "uint64_t" in json.dumps(hover), hover

        queried_alias = source.index("Word", source.index("sizeof"))
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, queried_alias + 1),
                },
            }
        )
        definition = session.receive_until(lambda message: message.get("id") == 4)[
            "result"
        ]
        assert definition, definition
        assert definition["range"]["start"] == lsp_position(
            source, source.index("Word")
        ), definition

        precise_use = source.index("precise")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 50,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, precise_use + 1),
                },
            }
        )
        precise_hover = session.receive_until(
            lambda message: message.get("id") == 50
        )["result"]
        assert precise_hover and "double" in json.dumps(precise_hover), precise_hover

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/formatting",
                "params": {
                    "textDocument": {"uri": uri},
                    "options": {"tabSize": 2, "insertSpaces": True},
                },
            }
        )
        edits = session.receive_until(lambda message: message.get("id") == 5)[
            "result"
        ]
        assert edits and "sizeof(Word)" in edits[0]["newText"], edits

        invalid_source = (
            "class Record { int value = 0; };\n"
            "uint64_t bad = sizeof(Record);\n"
        )
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
        invalid = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]
        layout_diagnostic = next(
            diagnostic
            for diagnostic in invalid["diagnostics"]
            if diagnostic.get("code") == "GTI-S2063"
        )
        queried_record = invalid_source.rindex("Record")
        assert layout_diagnostic["range"] == {
            "start": lsp_position(invalid_source, queried_record),
            "end": lsp_position(invalid_source, queried_record + len("Record")),
        }, layout_diagnostic
        assert "fixes" not in layout_diagnostic.get("data", {}), layout_diagnostic

        incomplete_source = "uint64_t broken = sizeof(\nint intact = 1;\n"
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": incomplete_source}],
                },
            }
        )
        incomplete = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
        )["params"]
        assert any(
            diagnostic.get("code") == "GTI-P0001"
            for diagnostic in incomplete["diagnostics"]
        ), incomplete
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/formatting",
                "params": {
                    "textDocument": {"uri": uri},
                    "options": {"tabSize": 2, "insertSpaces": True},
                },
            }
        )
        session.receive_until(lambda message: message.get("id") == 6)
    finally:
        session.close()


def test_integer_arithmetic_tooling(executable, root):
    source = (
        "#include <std/numeric>\n"
        "int main() {\n"
        "  auto result = std::checked_add(int8_t(1), int8_t(2));\n"
        "  uint8_t wrapped = std::wrapping_add(uint8_t(255), uint8_t(1));\n"
        "  return int32_t(result.value_or(int8_t(0))) + int32_t(wrapped);\n"
        "}\n"
    )
    path = root / "checked-integer-tooling.gti"
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
                            "hover": {"contentFormat": ["markdown"]},
                            "completion": {
                                "completionItem": {"snippetSupport": True}
                            },
                        }
                    }
                },
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
        publication = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 1
        )
        assert publication["params"]["diagnostics"] == [], publication

        selected = source.index("checked_add")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, selected + 1),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        rendered = json.dumps(hover)
        assert "checked_add" in rendered, hover
        assert "expected<int8_t, std::arithmetic_errc>" in rendered, hover

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, selected + 1),
                },
            }
        )
        definition = session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ]
        assert definition and definition["uri"].endswith("/std/numeric.gti"), definition

        wrapping = source.index("wrapping_add")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 30,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, wrapping + 1),
                },
            }
        )
        wrapping_hover = session.receive_until(
            lambda message: message.get("id") == 30
        )["result"]
        wrapping_rendered = json.dumps(wrapping_hover)
        assert "uint8_t std::wrapping_add" in wrapping_rendered, wrapping_hover
        assert "uint8_t left, uint8_t right" in wrapping_rendered, wrapping_hover

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 31,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, wrapping + 1),
                },
            }
        )
        wrapping_definition = session.receive_until(
            lambda message: message.get("id") == 31
        )["result"]
        assert wrapping_definition and wrapping_definition["uri"].endswith(
            "/std/numeric.gti"
        ), wrapping_definition

        completion_source = source.replace(
            "std::checked_add(int8_t(1), int8_t(2))", "std::checked_"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": completion_source}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )
        prefix = completion_source.index("std::checked_") + len("std::checked_")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(completion_source, prefix),
                },
            }
        )
        completion = session.receive_until(lambda message: message.get("id") == 4)[
            "result"
        ]
        labels = {item["label"] for item in completion["items"]}
        assert {"checked_add", "checked_sub", "checked_mul"} <= labels, completion
    finally:
        session.close()


def test_owned_move_capture_tooling(executable, root):
    source = (
        "#include <std/memory>\n"
        "class Owner { public: Owner() {} Owner(Owner& other) = delete; "
        "Owner(Owner&& other) = default; int read() { return 7; } };\n"
        "int main() { Owner source{}; "
        "auto action = [owned = std::move(source)]() -> int { "
        "return owned.read(); }; return action() - 7; }\n"
    )
    path = root / "owned-move-capture-tooling.gti"
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
                            "hover": {"contentFormat": ["markdown"]},
                            "publishDiagnostics": {"dataSupport": True},
                        }
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        token_types = initialization["capabilities"]["semanticTokensProvider"][
            "legend"
        ]["tokenTypes"]
        variable_type = token_types.index("variable")
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
        assert publication["params"]["diagnostics"] == [], publication

        capture_target = source.index("owned = std::move")
        capture_source = source.index("source)", capture_target)
        capture_body = source.index("owned.read")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, capture_body + 1),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert hover and "```gti\\nOwner\\n```" in json.dumps(hover), hover
        assert "owned move capture" in json.dumps(hover), hover
        assert hover["range"] == {
            "start": lsp_position(source, capture_body),
            "end": lsp_position(source, capture_body + len("owned")),
        }, hover

        for request_id, query_offset, expected_offset, spelling in (
            (3, capture_source, source.index("source{}"), "source"),
            (4, capture_body, capture_target, "owned"),
        ):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, query_offset + 1),
                    },
                }
            )
            definition = session.receive_until(
                lambda message, request_id=request_id: message.get("id")
                == request_id
            )["result"]
            assert definition and definition["uri"] == uri, definition
            assert definition["range"] == {
                "start": lsp_position(source, expected_offset),
                "end": lsp_position(source, expected_offset + len(spelling)),
            }, definition

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        tokens = semantic_tokens_by_position(
            session.receive_until(lambda message: message.get("id") == 5)[
                "result"
            ]["data"]
        )
        target_position = lsp_position(source, capture_target)
        source_position = lsp_position(source, capture_source)
        body_position = lsp_position(source, capture_body)
        assert tokens[
            (target_position["line"], target_position["character"])
        ]["type"] == variable_type
        assert tokens[
            (target_position["line"], target_position["character"])
        ]["modifiers"] & 4
        assert tokens[
            (source_position["line"], source_position["character"])
        ]["type"] == variable_type
        assert tokens[(body_position["line"], body_position["character"])][
            "type"
        ] == variable_type

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/formatting",
                "params": {
                    "textDocument": {"uri": uri},
                    "options": {"tabSize": 4, "insertSpaces": True},
                },
            }
        )
        formatting = session.receive_until(lambda message: message.get("id") == 6)[
            "result"
        ]
        assert formatting and (
            "[owned = std::move(source)]() -> int {"
            in formatting[0]["newText"]
        ), formatting

        invalid_source = (
            "int main() { int source = 1; "
            "auto invalid = [owned = source]() -> int { return owned; }; "
            "int recovered = 2; return recovered; }\n"
        )
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
        invalid = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]
        init_diagnostic = next(
            diagnostic
            for diagnostic in invalid["diagnostics"]
            if diagnostic.get("code") == "GTI-S2027"
            and "explicit owned-move form" in diagnostic["message"]
        )
        equal = invalid_source.index("= source", invalid_source.index("[owned"))
        assert init_diagnostic["range"] == {
            "start": lsp_position(invalid_source, equal),
            "end": lsp_position(invalid_source, equal + 1),
        }, init_diagnostic
    finally:
        session.close()


def test_native_record_tooling(executable, root):
    source = (
        "[[c_opaque]] struct NativeHandle;\n"
        "[[c_abi]]\n"
        "struct NativePoint {\n"
        "  mut float x;\n"
        "  mut float y;\n"
        "};\n"
        "extern \"C\" { NativeHandle* native_open(); "
        "void native_close(NativeHandle* handle); "
        "NativePoint point_roundtrip(NativePoint value); }\n"
        "int main() { return int32_t(sizeof(NativePoint) - uint64_t(8)); }\n"
    )
    path = root / "native-record-tooling.gti"
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
                            "hover": {
                                "contentFormat": ["markdown", "plaintext"]
                            },
                            "publishDiagnostics": {"dataSupport": True},
                        }
                    }
                },
            }
        )
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        token_types = initialization["capabilities"]["semanticTokensProvider"][
            "legend"
        ]["tokenTypes"]
        attribute_type = token_types.index("decorator")
        struct_type = token_types.index("struct")
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
        assert publication["params"]["diagnostics"] == [], publication

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(
                        source, source.index("NativePoint") + 1
                    ),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert hover and "[[c_abi]]" in json.dumps(hover), hover
        assert "size 8 bytes" in json.dumps(hover), hover
        assert "ABI alignment 4 bytes" in json.dumps(hover), hover

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 20,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(
                        source, source.index("NativeHandle") + 1
                    ),
                },
            }
        )
        opaque_hover = session.receive_until(
            lambda message: message.get("id") == 20
        )["result"]
        assert opaque_hover, opaque_hover
        opaque_hover_value = opaque_hover["contents"]["value"]
        assert opaque_hover_value.startswith(
            "```gti\n[[c_opaque]]\nstruct NativeHandle;\n```"
        ), opaque_hover
        assert "*opaque C handle: address-only through raw pointers*" in (
            opaque_hover_value
        ), opaque_hover
        assert "transfer-capable" not in opaque_hover_value, opaque_hover
        assert "share-capable" not in opaque_hover_value, opaque_hover
        opaque_name = source.index("NativeHandle")
        assert opaque_hover["range"] == {
            "start": lsp_position(source, opaque_name),
            "end": lsp_position(source, opaque_name + len("NativeHandle")),
        }, opaque_hover

        opaque_use = source.index(
            "NativeHandle", opaque_name + len("NativeHandle")
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 21,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, opaque_use + 1),
                },
            }
        )
        opaque_definition = session.receive_until(
            lambda message: message.get("id") == 21
        )["result"]
        assert opaque_definition and opaque_definition["uri"] == uri, (
            opaque_definition
        )
        assert opaque_definition["range"] == {
            "start": lsp_position(source, opaque_name),
            "end": lsp_position(source, opaque_name + len("NativeHandle")),
        }, opaque_definition

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        tokens = semantic_tokens_by_position(
            session.receive_until(lambda message: message.get("id") == 3)[
                "result"
            ]["data"]
        )
        attribute_position = lsp_position(source, source.index("c_abi"))
        assert tokens[(attribute_position["line"], attribute_position["character"])][
            "type"
        ] == attribute_type
        opaque_attribute_position = lsp_position(source, source.index("c_opaque"))
        assert tokens[
            (
                opaque_attribute_position["line"],
                opaque_attribute_position["character"],
            )
        ]["type"] == attribute_type
        opaque_name_position = lsp_position(source, opaque_name)
        assert tokens[
            (opaque_name_position["line"], opaque_name_position["character"])
        ]["type"] == struct_type

        invalid_source = (
            "[[c_abi]] struct Bad { bool value; };\n"
            "int main() { return 0; }\n"
        )
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
        invalid = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]
        diagnostic = next(
            item
            for item in invalid["diagnostics"]
            if item.get("code") == "GTI-S2064"
        )
        field_type = invalid_source.index("bool")
        assert diagnostic["range"] == {
            "start": lsp_position(invalid_source, field_type),
            "end": lsp_position(invalid_source, field_type + len("bool")),
        }, diagnostic
        assert "fixes" not in diagnostic.get("data", {}), diagnostic

        invalid_opaque_source = (
            "[[c_opaque]] struct Bad { int32_t value; };\n"
            "int main() { return 0; }\n"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": invalid_opaque_source}],
                },
            }
        )
        invalid_opaque = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 3
        )["params"]
        opaque_diagnostic = next(
            item
            for item in invalid_opaque["diagnostics"]
            if item.get("code") == "GTI-S2065"
        )
        opaque_name = invalid_opaque_source.index("Bad")
        assert opaque_diagnostic["range"] == {
            "start": lsp_position(invalid_opaque_source, opaque_name),
            "end": lsp_position(
                invalid_opaque_source, opaque_name + len("Bad")
            ),
        }, opaque_diagnostic
        assert "fixes" not in opaque_diagnostic.get("data", {}), opaque_diagnostic

        opaque_operation_source = (
            "[[c_opaque]] struct NativeHandle;\n"
            "extern \"C\" { NativeHandle* native_open(); }\n"
            "int main() { NativeHandle* handle = native_open(); "
            "unsafe { *handle; } return 0; }\n"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 4},
                    "contentChanges": [{"text": opaque_operation_source}],
                },
            }
        )
        invalid_opaque_operation = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 4
        )["params"]
        operation_diagnostic = next(
            item
            for item in invalid_opaque_operation["diagnostics"]
            if item.get("code") == "GTI-S2065"
        )
        dereference = opaque_operation_source.index("*handle")
        assert operation_diagnostic["range"] == {
            "start": lsp_position(opaque_operation_source, dereference),
            "end": lsp_position(opaque_operation_source, dereference + 1),
        }, operation_diagnostic
        assert "Opaque C handle" in operation_diagnostic["message"], (
            operation_diagnostic
        )
        assert operation_diagnostic["data"]["phase"] == "semantics", (
            operation_diagnostic
        )
        assert "fixes" not in operation_diagnostic.get("data", {}), (
            operation_diagnostic
        )

        incomplete_source = "[[c_abi]] struct Incomplete {\n  int32_t value;\n"
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 5},
                    "contentChanges": [{"text": incomplete_source}],
                },
            }
        )
        incomplete = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 5
        )["params"]
        assert any(
            item.get("code") == "GTI-P0001"
            for item in incomplete["diagnostics"]
        ), incomplete
    finally:
        session.close()


def test_protocol_input_validation(executable):
    for header in (
        b"Content-Length: -1\r\n\r\n",
        b"Content-Length: 16777217\r\n\r\n",
        b"Content-Length: 1x\r\n\r\n",
        b"Content-Length: 0\r\nContent-Length: 0\r\n\r\n",
    ):
        process = subprocess.run(
            [executable],
            input=header,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=5,
        )
        assert process.returncode == 0, process.stderr.decode(errors="replace")
        assert b"uncaught exception" not in process.stderr

    malformed = subprocess.run(
        [executable],
        input=b"Content-Length: 1\r\n\r\n{",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=5,
    )
    assert malformed.returncode == 0, malformed.stderr.decode(errors="replace")
    assert decode_messages(malformed.stdout)[0]["error"]["code"] == -32700

    invalid_utf8_payload = b'{"jsonrpc":"2.0","id":"\xff"}'
    invalid_utf8 = subprocess.run(
        [executable],
        input=(
            b"Content-Length: "
            + str(len(invalid_utf8_payload)).encode()
            + b"\r\n\r\n"
            + invalid_utf8_payload
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=5,
    )
    assert invalid_utf8.returncode == 0, invalid_utf8.stderr.decode(
        errors="replace"
    )
    assert decode_messages(invalid_utf8.stdout)[0]["error"]["code"] == -32700


def test_canonical_document_identity(executable, root):
    real_path = root / "canonical-library.gti"
    alias_path = root / "canonical-alias.gti"
    entry_path = root / "canonical-root.gti"
    dependency_source = "int overlay_value() { return 1; }\n"
    entry_source = (
        '#include "canonical-alias.gti"\n'
        "int main() { return overlay_value(); }\n"
    )
    real_path.write_text(dependency_source, encoding="utf-8")
    alias_path.symlink_to(real_path.name)
    entry_path.write_text(entry_source, encoding="utf-8")
    entry_uri = entry_path.resolve().as_uri()
    alias_uri = alias_path.absolute().as_uri()

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
        for uri, source in (
            (entry_uri, entry_source),
            (alias_uri, dependency_source),
        ):
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
                lambda message, expected=uri: message.get("method")
                == "textDocument/publishDiagnostics"
                and message.get("params", {}).get("uri") == expected
            )

        use = entry_source.index("overlay_value")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": entry_uri},
                    "position": lsp_position(entry_source, use + 1),
                },
            }
        )
        initial_definition = session.receive_until(
            lambda message: message.get("id") == 2
        )["result"]
        assert initial_definition["uri"] == alias_uri, initial_definition

        renamed_source = "int renamed_value() { return 1; }\n"
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": alias_uri, "version": 2},
                    "contentChanges": [{"text": renamed_source}],
                },
            }
        )
        root_diagnostics = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == entry_uri
            and any(
                diagnostic.get("code") == "GTI-S2001"
                for diagnostic in message.get("params", {}).get("diagnostics", [])
            )
        )
        assert "overlay_value" in root_diagnostics["params"]["diagnostics"][0][
            "message"
        ]
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": entry_uri},
                    "position": lsp_position(entry_source, use + 1),
                },
            }
        )
        assert session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ] is None
    finally:
        session.close()


def test_watched_dependency_reanalysis(executable, root):
    dependency_path = root / "watched-library.gti"
    entry_path = root / "watched-root.gti"
    dependency_source = "int watched_value() { return 1; }\n"
    entry_source = (
        '#include "watched-library.gti"\n'
        "int main() { return watched_value(); }\n"
    )
    dependency_path.write_text(dependency_source, encoding="utf-8")
    entry_path.write_text(entry_source, encoding="utf-8")
    entry_uri = entry_path.resolve().as_uri()

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
                            "didChangeWatchedFiles": {"dynamicRegistration": True}
                        }
                    }
                },
            }
        )
        session.receive_until(lambda message: message.get("id") == 1)
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        registration = session.receive_until(
            lambda message: message.get("method") == "client/registerCapability"
        )
        watchers = registration["params"]["registrations"][0]["registerOptions"][
            "watchers"
        ]
        assert watchers == [{"globPattern": "**/*.gti"}]
        session.send(
            {
                "jsonrpc": "2.0",
                "id": registration["id"],
                "result": None,
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": entry_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": entry_source,
                    }
                },
            }
        )
        initial = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == entry_uri
        )
        assert initial["params"]["diagnostics"] == []

        dependency_path.write_text(
            "int renamed_watched_value() { return 1; }\n", encoding="utf-8"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWatchedFiles",
                "params": {
                    "changes": [
                        {"uri": dependency_path.resolve().as_uri(), "type": 2}
                    ]
                },
            }
        )
        changed = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == entry_uri
            and any(
                diagnostic.get("code") == "GTI-S2001"
                for diagnostic in message.get("params", {}).get("diagnostics", [])
            )
        )
        assert "watched_value" in changed["params"]["diagnostics"][0]["message"]
    finally:
        session.close()


def test_pending_semantic_request_cancellation(executable, root):
    source = "".join(f"int queued_{index} = {index};\n" for index in range(8000))
    source += "int main() { return queued_7999; }\n"
    path = root / "cancel-semantic-request.gti"
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
        use = source.rindex("queued_7999")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, use + 1),
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "$/cancelRequest",
                "params": {"id": 2},
            }
        )
        canceled = session.receive_until(lambda message: message.get("id") == 2)
        assert canceled["error"]["code"] == -32800, canceled
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == uri
            and message.get("params", {}).get("version") == 1
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, use + 1),
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "$/cancelRequest",
                "params": {"id": 3},
            }
        )
        canceled_completion = session.receive_until(
            lambda message: message.get("id") == 3
        )
        assert canceled_completion["error"]["code"] == -32800, canceled_completion
    finally:
        session.close()


def test_worker_survives_failed_analysis(executable, root):
    """Analysis that cannot complete must leave the worker and its state usable.

    Publication happens outside the crash guard (docs/architecture/lsp.md), so
    a document whose analysis fails must not wedge `stateMutex` or the worker
    thread. This drives several failing documents through the same worker and
    then requires ordinary analysis and a semantic request to still succeed.
    """
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

        # Documents that fail source loading: the include target never exists,
        # so analysis stops before producing a snapshot.
        for index in range(3):
            broken_path = root / f"worker-failure-{index}.gti"
            broken_source = (
                f'#include "definitely-missing-{index}.gti"\n'
                "int main() { return 0; }\n"
            )
            broken_path.write_text(broken_source, encoding="utf-8")
            broken_uri = broken_path.resolve().as_uri()
            session.send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": broken_uri,
                            "languageId": "gti",
                            "version": 1,
                            "text": broken_source,
                        }
                    },
                }
            )
            published = session.receive_until(
                lambda message: message.get("method")
                == "textDocument/publishDiagnostics"
                and message.get("params", {}).get("uri") == broken_uri
            )
            assert published["params"]["diagnostics"], published

        # The worker must still analyze a healthy document afterwards.
        healthy_source = "int32_t answer() { return 42; }\nint main() { return 0; }\n"
        healthy_path = root / "worker-recovered.gti"
        healthy_path.write_text(healthy_source, encoding="utf-8")
        healthy_uri = healthy_path.resolve().as_uri()
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": healthy_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": healthy_source,
                    }
                },
            }
        )
        recovered = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == healthy_uri
        )
        assert recovered["params"]["diagnostics"] == [], recovered

        # And shared state must still serve a semantic request, which proves
        # the snapshot map and its mutex were never left locked.
        use = healthy_source.index("answer")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": healthy_uri},
                    "position": lsp_position(healthy_source, use + 1),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 2)
        assert hover.get("result"), hover
    finally:
        session.close()


def test_contextual_array_hover(executable, root):
    source = (
        "int total<uint64_t N>(int values[N]) { return values[0]; }\n"
        "class Total {\n"
        "public:\n"
        "  Total<uint64_t N>(int values[N]) {}\n"
        "};\n"
        "int main() {\n"
        "  int value = total({1, 2, 3});\n"
        "  Total object = Total({4, 5});\n"
        "  return value - 1;\n"
        "}\n"
    )
    path = root / "contextual-array-hover.gti"
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
                            "hover": {"contentFormat": ["markdown"]}
                        }
                    }
                },
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

        def hover(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]["contents"]["value"]

        declared_function = hover(2, source.index("total<uint64_t") + 1)
        assert declared_function.startswith(
            "```gti\nint32_t total<uint64_t N>(int32_t[N] values)\n```"
        ), declared_function

        selected_function = hover(3, source.index("total({") + 1)
        assert selected_function.startswith(
            "```gti\nint32_t total<3>(int32_t[3] values)\n```"
        ), selected_function

        declared_constructor = hover(4, source.index("Total<uint64_t") + 1)
        assert declared_constructor.startswith(
            "```gti\nTotal<uint64_t N>(int32_t[N] values)\n```"
        ), declared_constructor

        selected_constructor = hover(5, source.index("Total({") + 1)
        assert selected_constructor.startswith(
            "```gti\nTotal::Total<2>(int32_t[2] values)\n```"
        ), selected_constructor
    finally:
        session.close()


def test_pack_fold_tooling(executable, root):
    source = (
        "void visit<Value>(uint64_t& fixed, Value& value) {}\n"
        "void visit_all<Values...>(uint64_t& fixed, Values... values) {\n"
        "  (visit(fixed, values), ...);\n"
        "}\n"
    )
    path = root / "pack-fold-tooling.gti"
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
        initialization = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]
        token_types = initialization["capabilities"]["semanticTokensProvider"][
            "legend"
        ]["tokenTypes"]
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

        pack_declaration = source.index(
            "values)", source.index("void visit_all")
        )
        pack_occurrence = source.index("values), ...")

        def request(request_id, method, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": method,
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        hover = request(2, "textDocument/hover", pack_occurrence + 1)
        assert hover is not None, hover
        assert hover["contents"] == {
            "kind": "plaintext",
            "value": "Values...",
        }, hover
        assert hover["range"] == {
            "start": lsp_position(source, pack_occurrence),
            "end": lsp_position(source, pack_occurrence + len("values")),
        }

        definition = request(3, "textDocument/definition", pack_occurrence + 1)
        assert definition == {
            "uri": uri,
            "range": {
                "start": lsp_position(source, pack_declaration),
                "end": lsp_position(source, pack_declaration + len("values")),
            },
        }, definition

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        tokens = semantic_tokens_by_position(
            session.receive_until(lambda message: message.get("id") == 4)[
                "result"
            ]["data"]
        )
        occurrence_position = lsp_position(source, pack_occurrence)
        occurrence_token = tokens[
            (occurrence_position["line"], occurrence_position["character"])
        ]
        assert token_types[occurrence_token["type"]] == "parameter", (
            token_types,
            occurrence_token,
        )

        truncated = source.replace("), ...);", "), ...;")
        truncated_occurrence = truncated.index("values), ...")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": truncated}],
                },
            }
        )
        diagnostics = session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
        )["params"]["diagnostics"]
        assert any(
            item.get("code") == "GTI-P0001"
            and "after the pack fold ellipsis" in item.get("message", "")
            for item in diagnostics
        ), diagnostics

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(truncated, truncated_occurrence + 1),
                },
            }
        )
        assert session.receive_until(lambda message: message.get("id") == 5)[
            "result"
        ] is None

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        recovered_tokens = session.receive_until(
            lambda message: message.get("id") == 6
        )["result"]["data"]
        assert recovered_tokens
    finally:
        session.close()


def open_document(session, uri, source, expect_clean=True, version=1):
    session.send(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": uri,
                    "languageId": "gti",
                    "version": version,
                    "text": source,
                }
            },
        }
    )
    publication = session.receive_until(
        lambda message: message.get("method") == "textDocument/publishDiagnostics"
        and message["params"]["uri"] == uri
        and message["params"].get("version") == version
    )
    if expect_clean:
        assert publication["params"]["diagnostics"] == [], publication
    return publication


def test_references_and_highlights(executable, root):
    dependency_source = (
        "namespace math {\n"
        "uint64_t twice(uint64_t value) { return value + value; }\n"
        "}\n"
    )
    dependency_path = root / "references-library.gti"
    dependency_path.write_text(dependency_source, encoding="utf-8")
    source = (
        '#include "references-library.gti"\n'
        "int main() {\n"
        "  mut uint64_t total = uint64_t(0);\n"
        "  total = math::twice(total);\n"
        "  uint64_t copy = total;\n"
        "  return int(copy);\n"
        "}\n"
    )
    path = root / "references-tooling.gti"
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
        assert capabilities["referencesProvider"] is True
        assert capabilities["documentHighlightProvider"] is True
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        open_document(session, uri, source)

        declaration = source.index("total = uint64_t(0)")
        write = source.index("total = math")
        call_argument = source.index("total)")
        read = source.index("total;\n")

        def references(request_id, offset, include_declaration):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/references",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                        "context": {"includeDeclaration": include_declaration},
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        local_references = references(2, write + 1, True)
        assert [entry["range"]["start"] for entry in local_references] == [
            lsp_position(source, declaration),
            lsp_position(source, write),
            lsp_position(source, call_argument),
            lsp_position(source, read),
        ], local_references
        assert all(entry["uri"] == uri for entry in local_references)

        # The declaration site is excluded when the client does not ask
        # for it.
        assert [
            entry["range"]["start"]
            for entry in references(3, declaration + 1, False)
        ] == [
            lsp_position(source, write),
            lsp_position(source, call_argument),
            lsp_position(source, read),
        ]

        call = source.index("twice(total)")
        function_references = references(4, call + 1, True)
        dependency_declaration = dependency_source.index("twice")
        assert function_references == [
            {
                "uri": dependency_path.resolve().as_uri(),
                "range": {
                    "start": lsp_position(
                        dependency_source, dependency_declaration
                    ),
                    "end": lsp_position(
                        dependency_source, dependency_declaration + len("twice")
                    ),
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": lsp_position(source, call),
                    "end": lsp_position(source, call + len("twice")),
                },
            },
        ], function_references
        # Cross-file declarations stay out of the result when excluded; the
        # remaining site is the call in this document.
        assert [entry["uri"] for entry in references(5, call + 1, False)] == [
            uri
        ]

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/documentHighlight",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, read + 1),
                },
            }
        )
        highlights = session.receive_until(
            lambda message: message.get("id") == 6
        )["result"]
        assert [
            (entry["range"]["start"], entry["kind"]) for entry in highlights
        ] == [
            (lsp_position(source, declaration), 2),
            (lsp_position(source, write), 3),
            (lsp_position(source, call_argument), 2),
            (lsp_position(source, read), 2),
        ], highlights

        # Highlights for the dependency function stay inside this document.
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/documentHighlight",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, call + 1),
                },
            }
        )
        function_highlights = session.receive_until(
            lambda message: message.get("id") == 7
        )["result"]
        assert [entry["range"]["start"] for entry in function_highlights] == [
            lsp_position(source, call)
        ]

        # No symbol under the cursor produces a null result, not a guess.
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, source.index("{\n")),
                    "context": {"includeDeclaration": True},
                },
            }
        )
        assert (
            session.receive_until(lambda message: message.get("id") == 8)[
                "result"
            ]
            is None
        )

        # An unsaved dependency overlay shifts the declaration; the queued
        # references request answers from the reanalyzed snapshot.
        dependency_uri = dependency_path.resolve().as_uri()
        overlay_source = "// shifted\n" + dependency_source
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": dependency_uri,
                        "languageId": "gti",
                        "version": 1,
                        "text": overlay_source,
                    }
                },
            }
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": lsp_position(source, call + 1),
                    "context": {"includeDeclaration": True},
                },
            }
        )
        overlay_references = session.receive_until(
            lambda message: message.get("id") == 9
        )["result"]
        overlay_declaration = overlay_source.index("twice")
        assert overlay_references[0] == {
            "uri": dependency_uri,
            "range": {
                "start": lsp_position(overlay_source, overlay_declaration),
                "end": lsp_position(
                    overlay_source, overlay_declaration + len("twice")
                ),
            },
        }, overlay_references
    finally:
        session.close()


def test_document_symbols(executable, root):
    source = (
        "namespace engine {\n"
        "class Sprite {\n"
        "  /* αβ */ int frames = 0;\n"
        "public:\n"
        "  Sprite(int frames) : frames(frames) {}\n"
        "  int count() { return this.frames; }\n"
        "};\n"
        "}\n"
        "enum class Mode : uint8_t { Idle, Busy, };\n"
        "using Ticks = uint64_t;\n"
        "uint64_t elapsed = uint64_t(0);\n"
        "int main() { return 0; }\n"
    )
    path = root / "document-symbols.gti"
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
                            "documentSymbol": {
                                "hierarchicalDocumentSymbolSupport": True
                            }
                        }
                    }
                },
            }
        )
        capabilities = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]["capabilities"]
        assert capabilities["documentSymbolProvider"] is True
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

        def document_symbols(request_id):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/documentSymbol",
                    "params": {"textDocument": {"uri": uri}},
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        # Request immediately after didOpen: the request queues until the
        # document's first analysis publishes a snapshot.
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
        outline = document_symbols(2)
        assert [(symbol["name"], symbol["kind"]) for symbol in outline] == [
            ("engine", 3),
            ("Mode", 10),
            ("Ticks", 5),
            ("elapsed", 13),
            ("main", 12),
        ], outline

        namespace = outline[0]
        namespace_start = source.index("namespace engine")
        assert namespace["range"]["start"] == lsp_position(
            source, namespace_start
        )
        assert namespace["range"]["end"] == lsp_position(
            source, source.index("}\nenum class") + 1
        )
        sprite = namespace["children"][0]
        assert (sprite["name"], sprite["kind"]) == ("Sprite", 5)
        sprite_start = source.index("class Sprite")
        sprite_end = source.index("};\n}\n") + len("};")
        assert sprite["range"] == {
            "start": lsp_position(source, sprite_start),
            "end": lsp_position(source, sprite_end),
        }
        name_offset = source.index("Sprite", sprite_start)
        assert sprite["selectionRange"] == {
            "start": lsp_position(source, name_offset),
            "end": lsp_position(source, name_offset + len("Sprite")),
        }
        assert [
            (child["name"], child["kind"]) for child in sprite["children"]
        ] == [("frames", 8), ("Sprite", 9), ("count", 6)], sprite["children"]
        # The member on the comment line converts to UTF-16 columns.
        frames = sprite["children"][0]
        frames_offset = source.index("frames = 0")
        assert frames["selectionRange"]["start"] == lsp_position(
            source, frames_offset
        )
        assert frames["detail"] == "int32_t frames"

        mode = outline[1]
        assert [
            (child["name"], child["kind"]) for child in mode["children"]
        ] == [("Idle", 22), ("Busy", 22)]
        assert outline[4]["detail"].startswith("int"), outline[4]

        # Broken source still outlines the declarations the parser recovered.
        truncated = source + "class Later {\n"
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": truncated}],
                },
            }
        )
        session.receive_until(
            lambda message: message.get("method")
            == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == uri
            and message["params"].get("version") == 2
            and message["params"]["diagnostics"]
        )
        recovered = document_symbols(3)
        assert [symbol["name"] for symbol in recovered][:5] == [
            "engine",
            "Mode",
            "Ticks",
            "elapsed",
            "main",
        ], recovered
    finally:
        session.close()


def test_flat_document_symbols(executable, root):
    source = (
        "class Widget { public: int size() { return 1; } };\n"
        "int main() { return 0; }\n"
    )
    path = root / "flat-document-symbols.gti"
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
        open_document(session, uri, source)

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        flat = session.receive_until(lambda message: message.get("id") == 2)[
            "result"
        ]
        assert [
            (
                symbol["name"],
                symbol["kind"],
                symbol.get("containerName"),
                symbol["location"]["uri"],
            )
            for symbol in flat
        ] == [
            ("Widget", 5, None, uri),
            ("size", 6, "Widget", uri),
            ("main", 12, None, uri),
        ], flat
    finally:
        session.close()


def test_rename_tooling(executable, root):
    source = (
        "int helper(int value) { return value; }\n"
        "int main() {\n"
        "  mut int count = 0;\n"
        "  count = helper(count);\n"
        "  int limit = 3;\n"
        "  auto snapshot = [count]() -> int { return count; };\n"
        "  return snapshot() - count + limit;\n"
        "}\n"
    )
    path = root / "rename-tooling.gti"
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
                        "workspace": {"workspaceEdit": {"documentChanges": True}}
                    }
                },
            }
        )
        capabilities = session.receive_until(
            lambda message: message.get("id") == 1
        )["result"]["capabilities"]
        assert capabilities["renameProvider"] == {"prepareProvider": True}
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        open_document(session, uri, source)

        declaration = source.index("count = 0")

        def prepare(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/prepareRename",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        def rename(request_id, offset, new_name):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/rename",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                        "newName": new_name,
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )

        preparation = prepare(2, declaration + 1)
        assert preparation == {
            "range": {
                "start": lsp_position(source, declaration),
                "end": lsp_position(source, declaration + len("count")),
            },
            "placeholder": "count",
        }, preparation

        # Renaming a function fails closed: references may exist outside the
        # snapshot. prepareRename reports the same position as non-renamable.
        helper_declaration = source.index("helper")
        assert prepare(3, helper_declaration + 1) is None
        failure = rename(4, helper_declaration + 1, "renamed")
        assert "function-local" in failure["error"]["message"], failure

        # Invalid, keyword, reserved, and colliding names are rejected.
        for request_id, bad_name, fragment in (
            (5, "not an identifier", "not a valid GTI identifier"),
            (6, "class", "not a valid GTI identifier"),
            (7, "mutable", "reserved C++ core keyword"),
            (8, "limit", "already used"),
            (9, "helper", "already used"),
        ):
            failure = rename(request_id, declaration + 1, bad_name)
            assert fragment in failure["error"]["message"], failure

        # A captured local renames together with its copy-snapshot capture.
        renamed = rename(10, declaration + 1, "total")
        changes = renamed["result"]["documentChanges"]
        assert len(changes) == 1, renamed
        assert changes[0]["textDocument"] == {"uri": uri, "version": 1}
        edited = source
        offsets = []
        for edit in changes[0]["edits"]:
            assert edit["newText"] == "total"
            start = edit["range"]["start"]
            line_offset = 0
            for _ in range(start["line"]):
                line_offset = source.index("\n", line_offset) + 1
            # The rename source is ASCII, so UTF-16 columns equal byte offsets.
            offsets.append(line_offset + start["character"])
        expected_offsets = [
            declaration,
            source.index("count = helper"),
            source.index("count)"),
            source.index("count]"),
            source.index("count; }"),
            source.index("count + limit"),
        ]
        assert offsets == expected_offsets, (offsets, expected_offsets)
        for offset in sorted(offsets, reverse=True):
            edited = edited[:offset] + "total" + edited[offset + len("count") :]
        assert "count" not in edited
        assert "[total]() -> int { return total; }" in edited
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
    test_protocol_input_validation(sys.argv[1])

    directory = tempfile.TemporaryDirectory(prefix="gti-lsp-test-")
    root = pathlib.Path(directory.name)
    test_canonical_document_identity(sys.argv[1], root)
    test_watched_dependency_reanalysis(sys.argv[1], root)
    test_worker_survives_failed_analysis(sys.argv[1], root)
    test_pending_semantic_request_cancellation(sys.argv[1], root)
    test_semantic_hover(sys.argv[1], root)
    test_callable_contract_hover(sys.argv[1], root)
    test_requires_tooling(sys.argv[1], root)
    test_contextual_array_hover(sys.argv[1], root)
    test_semantic_definition(sys.argv[1], root)
    test_semantic_completion_and_parameter_tokens(sys.argv[1], root)
    test_compiler_private_tooling_boundary(sys.argv[1], root)
    test_diagnostic_capability_negotiation(sys.argv[1], root)
    test_current_language_diagnostics(sys.argv[1], root)
    test_unique_ptr_null_state_tooling(sys.argv[1], root)
    test_global_borrow_return_diagnostics(sys.argv[1], root)
    test_cpp_reserved_identifier_diagnostic(sys.argv[1], root)
    test_contextual_signed_integer_diagnostic(sys.argv[1], root)
    test_layout_query_tooling(sys.argv[1], root)
    test_native_record_tooling(sys.argv[1], root)
    test_integer_arithmetic_tooling(sys.argv[1], root)
    test_owned_move_capture_tooling(sys.argv[1], root)
    test_pack_fold_tooling(sys.argv[1], root)
    test_diagnostic_code_actions(sys.argv[1], root)
    test_references_and_highlights(sys.argv[1], root)
    test_document_symbols(sys.argv[1], root)
    test_flat_document_symbols(sys.argv[1], root)
    test_rename_tooling(sys.argv[1], root)
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
        "constexpr uint64_t compile_extent = uint64_t(2 + 2);\n"
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
            "params": {
                "capabilities": {
                    "textDocument": {
                        "publishDiagnostics": {
                            "relatedInformation": True,
                            "dataSupport": True,
                        }
                    }
                }
            },
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
        "constexpr",
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
    assert enum_type_token == 18, enum_type_token
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
    if constraint_namespace_key in token_types_by_position:
        assert token_types_by_position[constraint_namespace_key] == 3
        assert token_modifiers_by_position[constraint_namespace_key] & 8
    constraint_name_position = lsp_position(source, constraint + len("std::"))
    constraint_name_key = (
        constraint_name_position["line"],
        constraint_name_position["character"],
    )
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
    ] == 4
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
    assert "T& get() {" in formatted
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
    assert "int& box_value = box.get();" in formatted
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
    assert "int inspect_pixel(Pixel& pixel) {" in formatted
    assert "mut std::unique_ptr<Pixel> owner = std::make_unique<Pixel>(1);" in formatted
    assert "auto copied_owner = moved;" in formatted
    assert "int moved_value = owner->x;" in formatted
    assert "int invalid_array = buffer[3];" in formatted
    assert "uint64_t buffer_size = buffer.size();" in formatted
    assert "struct Pixel {\npublic:\n    mut int x;\n    Pixel(int x) : x(x) {}" in formatted
    assert "void reset() mut {\n        this.x = 0;\n    }" in formatted
    assert "~Pixel() {\n        this.reset();\n    }\nprivate:" in formatted
    assert "mut Pixel& operator->() mut {" in formatted
    assert "mut int& operator*() mut {" in formatted
    assert "mut int& operator[](uint64_t index) mut {" in formatted
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


def test_signature_help_tooling(executable, root):
    source = (
        "class Box {\n"
        "public:\n"
        "  Box(int width, int height) {}\n"
        "};\n"
        "int inner(int value) { return value; }\n"
        "int combine(int left, int right) { return left + right; }\n"
        "int main() {\n"
        '  std::print("\U0001F642"); int total = combine(1, inner(2));\n'
        "  Box box = Box(3, 4);\n"
        "  return total;\n"
        "}\n"
    )
    path = root / "signature-help.gti"
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
        initialized = session.receive_until(lambda message: message.get("id") == 1)
        capabilities = initialized["result"]["capabilities"]
        assert capabilities["signatureHelpProvider"]["triggerCharacters"] == [
            "(",
            ",",
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

        def help_at(request_id, offset):
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/signatureHelp",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, offset),
                    },
                }
            )
            return session.receive_until(
                lambda message: message.get("id") == request_id
            )["result"]

        # The compiler-selected overload with the active argument derived
        # from recorded separators; the emoji forces UTF-16 conversion.
        call = source.index("combine(1")
        first_argument = help_at(2, call + len("combine("))
        assert first_argument == {
            "signatures": [
                {
                    "label": "int32_t combine(int32_t left, int32_t right)",
                    "parameters": [
                        {"label": "int32_t left"},
                        {"label": "int32_t right"},
                    ],
                }
            ],
            "activeSignature": 0,
            "activeParameter": 0,
        }
        second_argument = help_at(3, source.index("inner(2)") - 1)
        assert second_argument["activeParameter"] == 1

        # The innermost call wins for a nested argument position.
        nested = help_at(4, source.index("inner(2)") + len("inner("))
        assert nested["signatures"][0]["label"] == "int32_t inner(int32_t value)"
        assert nested["activeParameter"] == 0

        # Constructor calls use the compiler-selected constructor.
        construction = help_at(5, source.index("Box(3") + len("Box("))
        assert construction["signatures"][0]["label"] == (
            "Box(int32_t width, int32_t height)"
        )
        assert (
            help_at(6, source.index("Box(3") + len("Box(3, "))["activeParameter"]
            == 1
        )

        # Outside any argument list there is no signature to offer.
        assert help_at(7, source.index("int main")) is None
    finally:
        session.close()
