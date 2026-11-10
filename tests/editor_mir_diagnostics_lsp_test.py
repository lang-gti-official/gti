#!/usr/bin/env python3
"""Editor snapshots run MIR and publish its diagnostics exactly as the compiler.

Body-local borrow and lifetime checking belongs to MIR alone, so an editor
snapshot must reach MIR for the open file's bodies. This test checks that the
analysis reaches the MIR stage (frontend trace), that a valid program using
standard-library instances of a user type publishes nothing (no internal MIR
error), and that a body-local borrow rejection is published with the same
code, primary location and related locations as a direct compilation.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


VALID = """import std.vector;
import std.string;
struct Item {
  int value;
  std::string name;
  Item(int value, std::string name) : value(value), name(std::move(name)) {}
};
int total(std::vector<Item>& items) {
  mut int sum = 0;
  for (Item& item : items) { sum = sum + item.value; }
  return sum;
}
int main() {
  mut std::vector<Item> items{};
  items.push_back(Item(1, std::string("one")));
  items.push_back(Item(2, std::string("two")));
  return total(items) == 3 ? 0 : 1;
}
"""

# A parent loan used while an overlapping mutable child reborrow is live.
CONFLICT = """void keep(int& value) {}
int main() { mut int owner = 1; mut int& parent = owner;
  mut int& child = parent; keep(parent); keep(child); return 0; }
"""

LOCATION = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): "
                      r"(?P<kind>error|warning|note)(?:\[(?P<code>GTI-[A-Z]\d+)\])?:")


def compiler_diagnostics(compiler, path, scratch):
    """(code, line, character, related positions) for each compiler error."""
    completed = subprocess.run(
        [compiler, str(path), "--emit-mir", "-o", str(scratch / "out.mir"),
         "--color=never"],
        capture_output=True, text=True, check=False)
    result = []
    for line in (completed.stdout + completed.stderr).splitlines():
        match = LOCATION.match(line)
        if not match or pathlib.Path(match["path"]).resolve() != path.resolve():
            continue
        position = (int(match["line"]) - 1, int(match["column"]) - 1)
        if match["kind"] == "error":
            result.append([match["code"], position, []])
        elif match["kind"] == "note" and result:
            result[-1][2].append(position)
    return [(code, position, sorted(related)) for code, position, related in result]


def editor_diagnostics(items):
    result = []
    for item in items:
        if item.get("severity") != 1:
            continue
        start = item["range"]["start"]
        related = sorted(
            (entry["location"]["range"]["start"]["line"],
             entry["location"]["range"]["start"]["character"])
            for entry in item.get("relatedInformation", []))
        result.append((item.get("code"), (start["line"], start["character"]),
                       related))
    return result


def main():
    executable, compiler = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="gti-editor-mir-lsp-") as directory:
        root = pathlib.Path(directory).resolve()
        path = root / "main.gti"
        uri = path.as_uri()
        trace = root / "trace.jsonl"
        session = LspSession(executable, environment=dict(
            os.environ, GTI_LSP_UPDATE_DEBOUNCE_MS="0",
            GTI_LSP_FRONTEND_TRACE=str(trace)))
        version = 0

        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti",
                                           "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version},
                          "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            reply = session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics" and
                item["params"].get("uri") == uri and
                item["params"].get("version") == version)
            return reply["params"]["diagnostics"]

        def analysis_record(generation_count):
            records = [json.loads(line) for line in trace.read_text().splitlines()
                       if line.strip()]
            analyses = [record for record in records
                        if record.get("kind") == "analysis" and
                        record.get("uri") == uri]
            assert len(analyses) >= generation_count, analyses
            return analyses[generation_count - 1]

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

            # A valid program whose library instances name a user type reaches
            # MIR and publishes nothing, in particular no internal MIR error.
            assert publish(VALID) == []
            record = analysis_record(1)
            assert record["mirMicroseconds"] > 0, record
            assert record["hirFunctionInstances"] > 0, record

            # A body-local borrow rejection: the editor publishes exactly the
            # compiler's code, primary position and related positions.
            path.write_text(CONFLICT)
            expected = compiler_diagnostics(compiler, path, root)
            assert expected, "the compiler must reject the conflict"
            published = publish(CONFLICT)
            assert editor_diagnostics(published) == expected, (published, expected)
            primary = next(item for item in published
                           if item.get("code") == expected[0][0])
            offset = CONFLICT.index("keep(parent)") + len("keep(")
            assert primary["range"] == {
                "start": lsp_position(CONFLICT, offset),
                "end": lsp_position(CONFLICT, offset + len("parent"))}, primary

            # Repairing the body clears the diagnostic in the next snapshot.
            repaired = CONFLICT.replace("keep(parent); keep(child);",
                                        "keep(child); keep(parent);")
            assert publish(repaired) == []
            assert analysis_record(3)["mirMicroseconds"] > 0
        finally:
            session.close()


if __name__ == "__main__":
    main()
