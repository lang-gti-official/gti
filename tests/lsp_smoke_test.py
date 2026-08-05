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


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_smoke_test.py /path/to/gti_lsp")

    directory = tempfile.TemporaryDirectory(prefix="gti-lsp-test-")
    root = pathlib.Path(directory.name)
    (root / "library.gti").write_text(
        "int identity(int value) { return value; }\n", encoding="utf-8"
    )
    uri = (root / "lsp-smoke.gti").as_uri()
    source = (
        'include "library.gti"\n'
        "namespace engine { namespace graphics { void render() {} } }\n"
        "namespace gfx = engine::graphics;\n"
        "expected<int, int> calculate(bool fail) { "
        "if (fail) { return unexpected(1); } return 2; }\n"
        'int main() { std::print("hello"); gfx::render(); '
        "calculate(false); int hello = identity(1); hello = 2; return 0; }\n"
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
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None},
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

    diagnostics = next(
        message["params"]["diagnostics"]
        for message in messages
        if message.get("method") == "textDocument/publishDiagnostics"
    )
    assert any("not assignable" in diagnostic["message"] for diagnostic in diagnostics)
    assert any(
        "Function return value must be used" in diagnostic["message"]
        for diagnostic in diagnostics
    )

    token_data = by_id[2]["result"]["data"]
    assert token_data and len(token_data) % 5 == 0
    assert token_data[3] == 0
    assert by_id[3]["result"] is None


if __name__ == "__main__":
    main()
