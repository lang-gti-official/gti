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


def test_unsaved_dependency_reanalysis(executable, root):
    library_path = root / "overlay-library.gti"
    root_path = root / "overlay-root.gti"
    disk_source = 'int overlay_value = "stale disk text";\n'
    valid_source = "int overlay_value = 1;\n"
    invalid_buffer_source = 'int overlay_value = "unsaved buffer text";\n'
    root_source = (
        'include "overlay-library.gti"\n'
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
        'include "visibility-leaf.gti"\n'
        "int visibility_branch() { return visibility_leaf(); }\n"
    )
    root_source = (
        'include "visibility-branch.gti"\n'
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
        assert 'include "visibility-leaf.gti"' in diagnostic["message"]
        assert diagnostic["relatedInformation"][0]["location"]["uri"] == leaf_uri
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
    library_source = (
        "T identity<T>(T value) { return value; }\n"
        'int dependency_value = "bad";\n'
    )
    library_path = root / "library.gti"
    library_path.write_text(library_source, encoding="utf-8")
    uri = (root / "lsp-smoke.gti").as_uri()
    stress_uri = (root / "lsp-stress.gti").as_uri()
    source = (
        'include "library.gti"\n'
        "include <std/array>\n"
        '#if target.os == "never"\n'
        "int inactive() { return missing_name; }\n"
        "#endif\n"
        "namespace engine { namespace graphics { void render() {} } }\n"
        "namespace gfx = engine::graphics;\n"
        "class Box<T> { T value; public: Box(T value) : value(value) {} "
        "T& get() { return self.value; } };\n"
        "class StaticArray<T, uint64 N> { T values[N] = {}; public: "
        "uint64 size() { return N; } };\n"
        "struct Pixel { public: mut int x; Pixel(int x) : x(x) {} "
        "void reset() mut { self.x = 0; } "
        "~Pixel() { self.reset(); } private: int y = 0; };\n"
        "class Handle { mut Pixel pixel = Pixel(1); mut int value = 0; public: "
        "Pixel& operator->() { return self.pixel; } "
        "mut Pixel& operator->() mut { return self.pixel; } "
        "int& operator*() { return self.value; } "
        "mut int& operator*() mut { return self.value; } "
        "int& operator[](uint64 index) { return self.value; } "
        "mut int& operator[](uint64 index) mut { return self.value; } "
        "bool operator==(nullptr_t other) { return false; } "
        "bool operator!=(nullptr_t other) { return true; } "
        "operator bool() { return true; } };\n"
        "struct Shade { int value = 0; Shade(int value) : value(value) {} "
        "Shade(bool reset) {} };\n"
        "int inspect_pixel(Pixel& pixel) { return pixel.x; }\n"
        "expected<int, int> calculate(bool fail) { "
        "if (fail) { return unexpected(1); } return 2; }\n"
        "uint64 overloaded(uint64 value) { return value; }\n"
        "float overloaded(float value) { return value; }\n"
        "void consume<Args...>(Args... values) {}\n"
        "void relay<Args...>(Args... values) { consume(values...); }\n"
        'int main() { std::print("\U0001F642"); gfx::render(); '
        "Box<int> box = Box<int>(identity(1)); "
        "StaticArray<int, 4> fixed = StaticArray<int, 4>(); "
        "uint64 fixed_size = fixed.size(); "
        "std::array<int, 3> standard_array = std::array<int, 3>(); "
        "uint64 standard_size = standard_array.size(); "
        "int& box_value = box.get(); "
        "int bits = ((identity(1) << 3) | 2) ^ 1; "
        "int remainder = bits % 3; int inverted = ~bits; "
        "auto add_offset = [fixed_size](uint64 value) -> uint64 { "
        "return fixed_size + value; }; "
        "uint64 lambda_value = add_offset(uint64(1)); "
        "mut Pixel pixel = Pixel(identity<int>(1)); pixel.reset(); "
        "mut Handle handle = Handle(); handle->reset(); *handle = 1; "
        "handle[uint64(0)] += 1; bool present = handle != nullptr; "
        "if (handle and present) { *handle += 1; } "
        "mut std::unique_ptr<Pixel> owner = std::make_unique<Pixel>(1); "
        "std::unique_ptr<Pixel> moved = std::move(owner); "
        "int borrowed = inspect_pixel(*moved); int moved_value = owner->x; "
        "uint64 exact = overloaded(uint64(1)); overloaded(1); "
        'Shade invalid_shade = Shade("bad"); '
        "mut int buffer[3] = {1, 2, 3}; buffer[1] += 2; "
        "int invalid_array = buffer[3]; uint64 buffer_size = buffer.size(); "
        "mut int iterations = 0; while (iterations < 2) { iterations++; "
        "if (iterations == 1) { continue; } break; } "
        "[[discard]] identity(1); calculate(false); int hello = identity(1); "
        "hello = 2; int8 small = 1; uint8 byte = 255; return 0; } "
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
    ]
    assert legend["tokenModifiers"] == [
        "declaration",
        "definition",
        "readonly",
        "defaultLibrary",
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
    assert len(diagnostics) == 6, diagnostics
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
    assert "exactly matches argument types (int32)" in overload["message"]
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
    assert "argument types (string)" in constructor_overload["message"]
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
    assert token_data[3] == 0
    assert {2, 4, 8, 9, 12, 13, 14, 15}.issubset(set(token_data[3::5]))
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
    for keyword in ("continue", "break"):
        position = lsp_position(source, source.index(keyword + ";"))
        assert token_types_by_position[(position["line"], position["character"])] == 0
    type_parameter = source.index("T", source.index("class Box<T>"))
    type_parameter_position = lsp_position(source, type_parameter)
    assert token_types_by_position[
        (type_parameter_position["line"], type_parameter_position["character"])
    ] == 2
    value_parameter = source.index("N", source.index("uint64 N"))
    value_parameter_position = lsp_position(source, value_parameter)
    assert token_types_by_position[
        (value_parameter_position["line"], value_parameter_position["character"])
    ] == 8
    assert token_modifiers_by_position[
        (value_parameter_position["line"], value_parameter_position["character"])
    ] & 4
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
    ] == 1
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

    formatting_edits = by_id[3]["result"]
    assert len(formatting_edits) == 1
    formatted = formatting_edits[0]["newText"]
    assert "namespace engine {\n    namespace graphics" in formatted
    assert "class Box<T> {" in formatted
    assert "T & get() {" in formatted
    assert "Box<int> box = Box<int>(identity(1));" in formatted
    assert "class StaticArray<T, uint64 N> {" in formatted
    assert "include <std/array>" in formatted
    assert "T values[N] = {};" in formatted
    assert "StaticArray<int, 4> fixed = StaticArray<int, 4>();" in formatted
    assert "std::array<int, 3> standard_array = std::array<int, 3>();" in formatted
    assert "int & box_value = box.get();" in formatted
    assert "identity<int>(1)" in formatted
    assert "int bits = ((identity(1) << 3) | 2) ^ 1;" in formatted
    assert "int remainder = bits % 3;" in formatted
    assert "int inverted = ~bits;" in formatted
    assert (
        "auto add_offset = [fixed_size](uint64 value) -> uint64 {" in formatted
    )
    assert "while (iterations < 2) {\n        iterations++;" in formatted
    assert "            continue;\n        }\n        break;" in formatted
    assert "expected<int, int> calculate(bool fail) {" in formatted
    assert "uint64 exact = overloaded(uint64(1));" in formatted
    assert "mut int buffer[3] = {1, 2, 3};" in formatted
    assert "int inspect_pixel(Pixel & pixel) {" in formatted
    assert "mut std::unique_ptr<Pixel> owner = std::make_unique<Pixel>(1);" in formatted
    assert "int moved_value = owner->x;" in formatted
    assert "int invalid_array = buffer[3];" in formatted
    assert "uint64 buffer_size = buffer.size();" in formatted
    assert "struct Pixel {\npublic:\n    mut int x;\n    Pixel(int x) : x(x) {}" in formatted
    assert "void reset() mut {\n        self.x = 0;\n    }" in formatted
    assert "~Pixel() {\n        self.reset();\n    }\nprivate:" in formatted
    assert "mut Pixel & operator->() mut {" in formatted
    assert "mut int & operator*() mut {" in formatted
    assert "mut int & operator[](uint64 index) mut {" in formatted
    assert "operator bool() {" in formatted
    assert "void relay<Args...>(Args... values) {" in formatted
    assert "consume(values...);" in formatted
    assert "handle[uint64(0)] += 1;" in formatted
    assert "        return unexpected(1);" in formatted
    assert "std::print(" in formatted
    assert "int8 small = 1;" in formatted
    assert "uint8 byte = 255;" in formatted
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


if __name__ == "__main__":
    main()
