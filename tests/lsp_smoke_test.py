#!/usr/bin/env python3

"""Protocol smoke test for the GTI language server."""

import json
import pathlib
import subprocess
import sys
import tempfile


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


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_smoke_test.py /path/to/gti_lsp")

    directory = tempfile.TemporaryDirectory(prefix="gti-lsp-test-")
    root = pathlib.Path(directory.name)
    library_source = (
        "T identity<T>(T value) { return value; }\n"
        'int dependency_value = "bad";\n'
    )
    library_path = root / "library.gti"
    library_path.write_text(library_source, encoding="utf-8")
    uri = (root / "lsp-smoke.gti").as_uri()
    source = (
        'include "library.gti"\n'
        '#if target.os == "never"\n'
        "int inactive() { return missing_name; }\n"
        "#endif\n"
        "namespace engine { namespace graphics { void render() {} } }\n"
        "namespace gfx = engine::graphics;\n"
        "class Box<T> { T value; public: Box(T value) : value(value) {} "
        "T get() { return self.value; } };\n"
        "struct Pixel { public: mut int x; Pixel(int x) : x(x) {} "
        "void reset() mut { self.x = 0; } private: int y = 0; };\n"
        "expected<int, int> calculate(bool fail) { "
        "if (fail) { return unexpected(1); } return 2; }\n"
        'int main() { std::print("\U0001F642"); gfx::render(); '
        "Box<int> box = Box<int>(identity(1)); "
        "int bits = ((identity(1) << 3) | 2) ^ 1; "
        "int remainder = bits % 3; int inverted = ~bits; "
        "mut Pixel pixel = Pixel(identity<int>(1)); pixel.reset(); "
        "mut int iterations = 0; while (iterations < 2) { iterations++; "
        "if (iterations == 1) { continue; } break; } "
        "[[discard]] identity(1); calculate(false); int hello = identity(1); "
        "hello = 2; int8 small = 1; uint8 byte = 255; return 0; } "
        "// entry point\n"
    )
    recovery_source = (
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
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": recovery_source}],
            },
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
    expected_version = (
        pathlib.Path(__file__).resolve().parent.parent / "VERSION"
    ).read_text(encoding="utf-8").strip()
    assert by_id[1]["result"]["serverInfo"]["version"] == expected_version

    legend = initialization["semanticTokensProvider"]["legend"]
    assert legend["tokenTypes"] == [
        "keyword",
        "type",
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
    initial_publication = next(
        params
        for params in publications
        if params["uri"] == uri and params.get("version") == 1
    )
    diagnostics = initial_publication["diagnostics"]
    assert len(diagnostics) == 2, diagnostics
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
    assert not any(
        "missing_name" in diagnostic["message"] for diagnostic in diagnostics
    )

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

    recovered_publication = next(
        params
        for params in publications
        if params["uri"] == uri and params.get("version") == 2
    )
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
    assert {3, 7, 8, 11, 12, 13, 14}.issubset(set(token_data[3::5]))
    assert any(modifier != 0 for modifier in token_data[4::5])

    token_types_by_position = {}
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
    for keyword in ("continue", "break"):
        position = lsp_position(source, source.index(keyword + ";"))
        assert token_types_by_position[(position["line"], position["character"])] == 0

    formatting_edits = by_id[3]["result"]
    assert len(formatting_edits) == 1
    formatted = formatting_edits[0]["newText"]
    assert "namespace engine {\n    namespace graphics" in formatted
    assert "class Box<T> {" in formatted
    assert "Box<int> box = Box<int>(identity(1));" in formatted
    assert "identity<int>(1)" in formatted
    assert "int bits = ((identity(1) << 3) | 2) ^ 1;" in formatted
    assert "int remainder = bits % 3;" in formatted
    assert "int inverted = ~bits;" in formatted
    assert "while (iterations < 2) {\n        iterations++;" in formatted
    assert "            continue;\n        }\n        break;" in formatted
    assert "expected<int, int> calculate(bool fail) {" in formatted
    assert "struct Pixel {\npublic:\n    mut int x;\n    Pixel(int x) : x(x) {}" in formatted
    assert "void reset() mut {\n        self.x = 0;\n    }\nprivate:" in formatted
    assert "        return unexpected(1);" in formatted
    assert "std::print(" in formatted
    assert "int8 small = 1;" in formatted
    assert "uint8 byte = 255;" in formatted
    assert formatted.endswith("// entry point\n")
    assert by_id[4]["result"] is None


if __name__ == "__main__":
    main()
