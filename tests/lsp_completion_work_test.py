#!/usr/bin/env python3
"""Completion reuses trusted analysis, cancels work, and rejects stale inputs."""

import json
import os
import pathlib
import sys
import tempfile
import time

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position


def change(session, uri, version, source):
    session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                  "params": {"textDocument": {"uri": uri, "version": version},
                             "contentChanges": [{"text": source}]}})


def complete(session, request_id, uri, source, marker="std::size_"):
    session.send({"jsonrpc": "2.0", "id": request_id,
                  "method": "textDocument/completion",
                  "params": {"textDocument": {"uri": uri},
                             "position": lsp_position(
                                 source, source.index(marker) + len(marker))}})


def responses(session, *ids):
    found = {}

    def collect(message):
        if message.get("id") in ids:
            found[message["id"]] = message
        return len(found) == len(ids)

    session.receive_until(collect)
    return found


def records(trace):
    if not trace.exists():
        return []
    # A concurrent writer may not have finished its last line yet.
    return [json.loads(line) for line in trace.read_text().splitlines(keepends=True)
            if line.endswith("\n")]


def preamble_built(trace):
    # The first analysis defers the standard-library preamble to the preamble
    # worker; completion adopts it only once that build is stored.
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        if any(row["kind"] == "preamble" and row["preambleBuilds"] == 1
               for row in records(trace)):
            return
        time.sleep(0.01)
    raise AssertionError(f"preamble never built: {records(trace)}")


def started(trace, request_id):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if any(row["kind"] == "completion-started"
               and row.get("requestId") == str(request_id) for row in records(trace)):
            return
        time.sleep(0.002)
    raise AssertionError(f"completion {request_id} never started: {records(trace)}")


def session_for(executable, trace):
    return LspSession(executable, environment=dict(
        os.environ, GTI_LSP_FRONTEND_TRACE=str(trace),
        GTI_LSP_AUDIT_QUIET_MS="600000", GTI_LSP_UPDATE_DEBOUNCE_MS="10000"))


SOURCE = ("import std.vector;\n"
          "int main() { std::size_t count = 0; return 0; }\n")
# Large enough to exercise an active request, rather than cancelling a queued
# request before the compiler starts. No elapsed-time acceptance threshold.
BUSY = SOURCE + "".join(f"int completion_value_{i} = {i};\n" for i in range(12000))


def reuse_and_diagnostics(executable, root):
    path, trace = root / "reuse.gti", root / "reuse.jsonl"
    write(path, SOURCE)
    session = session_for(executable, trace)
    try:
        initialize(session)
        uri = open_document(session, path, SOURCE)
        assert not diagnostics_for(session, uri, 1)
        preamble_built(trace)
        items = []
        for request_id in (10, 11):
            complete(session, request_id, uri, SOURCE)
            result = responses(session, request_id)[request_id]
            assert "error" not in result, result
            items.append(result["result"]["items"])
        assert items[0] == items[1] and items[0], items
        assert any(item["label"] == "size_t" for item in items[0]), items[0]
        completed = [row for row in records(trace) if row["kind"] == "completion"]
        assert len(completed) == 2, completed
        assert all(row["preambleAdoptions"] == 1 and row["preambleBuilds"] == 0
                   for row in completed), [
                       {key: row[key] for key in ("kind", "preambleAdoptions", "preambleBuilds")}
                       for row in completed]

        broken = SOURCE.replace("std::size_t", "st")
        change(session, uri, 2, broken)
        # A semantic query makes the queued analysis immediately eligible.
        session.send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/hover",
                      "params": {"textDocument": {"uri": uri},
                                 "position": {"line": 1, "character": 1}}})
        assert any("Unknown type 'st'" in item["message"]
                   for item in diagnostics_for(session, uri, 2))
        responses(session, 12)
        change(session, uri, 3, SOURCE)
        session.send({"jsonrpc": "2.0", "id": 13, "method": "textDocument/hover",
                      "params": {"textDocument": {"uri": uri},
                                 "position": {"line": 1, "character": 1}}})
        assert not diagnostics_for(session, uri, 3)
        responses(session, 13)
    finally:
        session.close()


def active_cancellation(executable, root, mode):
    path, trace = root / f"{mode}.gti", root / f"{mode}.jsonl"
    write(path, SOURCE)
    session = session_for(executable, trace)
    closed = False
    try:
        initialize(session)
        uri = open_document(session, path, SOURCE)
        assert not diagnostics_for(session, uri, 1)
        change(session, uri, 2, BUSY)
        complete(session, 20, uri, BUSY)
        started(trace, 20)
        expected = -32800
        ids = [20]
        if mode == "explicit":
            session.send({"jsonrpc": "2.0", "method": "$/cancelRequest",
                          "params": {"id": 20}})
        elif mode == "edit":
            change(session, uri, 3, SOURCE)
            expected = -32801
        elif mode == "reopen":
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            open_document(session, path, SOURCE, version=2)
            expected = -32801
        elif mode == "supersede":
            complete(session, 21, uri, BUSY)
            ids.append(21)
            expected = -32802
        elif mode == "shutdown":
            session.send({"jsonrpc": "2.0", "id": 22, "method": "shutdown",
                          "params": None})
            ids.append(22)
        else:
            raise AssertionError(mode)
        found = responses(session, *ids)
        assert found[20].get("error", {}).get("code") == expected, found
        if mode == "supersede":
            assert found[21].get("result", {}).get("items"), found[21]
        cancelled = [row for row in records(trace)
                     if row["kind"] == "completion-cancelled"
                     and row.get("requestId") == "20"]
        assert len(cancelled) == 1, records(trace)
        if mode == "shutdown":
            complete(session, 23, uri, BUSY)
            assert responses(session, 23)[23].get("error", {}).get("code") == -32600
            session.send({"jsonrpc": "2.0", "method": "exit", "params": None})
            session.process.wait(timeout=5)
            assert session.process.returncode == 0
            closed = True
    finally:
        if not closed:
            session.close()


def changed_new_dependency(executable, root, related, missing=False):
    project = root / ("missing" if missing else "related" if related else "unrelated")
    path, api, trace = project / "src/main.gti", project / "src/api.gti", project / "trace.jsonl"
    write(project / "gti.toml", 'manifest-version = 1\n[package]\nname = "completion"\n'
          'version = "0.1.0"\n[targets.demo]\nkind = "executable"\nroot = "src/main.gti"\n')
    write(path, SOURCE)
    api_source = "export module demo.api;\nexport int answer() { return 1; }\n"
    if not missing:
        write(api, api_source)
    session = session_for(executable, trace)
    try:
        initialize(session)
        uri = open_document(session, path, SOURCE)
        assert not diagnostics_for(session, uri, 1)
        # The published root graph does not yet contain this import. Completion
        # must validate the inputs it actually loaded, even before the deferred
        # diagnostic analysis has discovered the new dependency edge.
        source = ('import "api.gti";\n' if missing else "import demo.api;\n") + BUSY
        change(session, uri, 2, source)
        complete(session, 30, uri, source)
        started(trace, 30)
        changed_path = api if related else project / "src/other.gti"
        changed_source = api_source.replace("return 1", "return 2") if related else "int unrelated = 0;\n"
        open_document(session, changed_path, changed_source)
        result = responses(session, 30)[30]
        if related:
            assert result.get("error", {}).get("code") == -32801, result
        else:
            assert result.get("result", {}).get("items"), result
    finally:
        session.close()


def main():
    with tempfile.TemporaryDirectory(prefix="gti-completion-work-") as directory:
        root = pathlib.Path(directory)
        reuse_and_diagnostics(sys.argv[1], root)
        for mode in ("explicit", "edit", "reopen", "supersede", "shutdown"):
            active_cancellation(sys.argv[1], root, mode)
        changed_new_dependency(sys.argv[1], root, related=True)
        changed_new_dependency(sys.argv[1], root, related=False)
        changed_new_dependency(sys.argv[1], root, related=True, missing=True)


if __name__ == "__main__":
    main()
