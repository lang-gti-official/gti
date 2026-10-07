#!/usr/bin/env python3

"""Project/package graph integration tests for gti_lsp (T-LSP-04)."""

import json
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def initialize(session, watched_files=False):
    workspace = {}
    if watched_files:
        workspace["didChangeWatchedFiles"] = {"dynamicRegistration": True}
    session.send(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"capabilities": {"workspace": workspace}},
        }
    )
    initialized = session.receive_until(lambda message: message.get("id") == 1)
    session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    if watched_files:
        registration = session.receive_until(
            lambda message: message.get("method") == "client/registerCapability"
        )
        watchers = registration["params"]["registrations"][0]["registerOptions"][
            "watchers"
        ]
        assert watchers == [
            {"globPattern": "**/*.gti"},
            {"globPattern": "**/gti.toml"},
            {"globPattern": "**/gti.lock"},
        ], watchers
        session.send(
            {
                "jsonrpc": "2.0",
                "id": registration["id"],
                "result": None,
            }
        )
    return initialized


def open_document(session, path, source, version=1):
    uri = path.resolve().as_uri()
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
    return uri


def diagnostics_for(session, uri, version=None, predicate=None):
    def matches(message):
        if message.get("method") != "textDocument/publishDiagnostics":
            return False
        params = message.get("params", {})
        if params.get("uri") != uri:
            return False
        if version is not None and params.get("version") != version:
            return False
        return predicate is None or predicate(params.get("diagnostics", []))

    return session.receive_until(matches)["params"]["diagnostics"]


def completion_labels(session, request_id, uri, source):
    offset = source.index("counter.") + len("counter.")
    session.send(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": lsp_position(source, offset),
            },
        }
    )
    result = session.receive_until(lambda message: message.get("id") == request_id)[
        "result"
    ]
    return {item["label"] for item in result["items"]}


def test_path_dependency_tooling(executable, root):
    app = root / "app"
    dependency = root / "math"
    app_path = app / "src/main.gti"
    dependency_path = dependency / "lib/counter.gti"

    app_source = (
        "#include <math/counter>\n"
        "int main() {\n"
        "  math::Counter counter = math::Counter();\n"
        "  return counter.value();\n"
        "}\n"
    )
    dependency_source = (
        "#if defined(PROJECT_FEATURE)\n"
        "namespace math {\n"
        "class Counter {\n"
        "public:\n"
        "  int value() { return 7; }\n"
        "  int doubled() { return 14; }\n"
        "};\n"
        "}\n"
        "#endif\n"
    )
    write(app_path, app_source)
    write(dependency_path, dependency_source)
    write(
        app / "gti.toml",
        "manifest-version = 1\n\n"
        "[package]\n"
        'name = "app"\n'
        'version = "0.1.0"\n\n'
        "[dependencies]\n"
        'math = { path = "../math" }\n\n'
        "[targets.app]\n"
        'kind = "executable"\n'
        'root = "src/main.gti"\n',
    )
    write(
        dependency / "gti.toml",
        "manifest-version = 1\n\n"
        "[package]\n"
        'name = "math"\n'
        'version = "1.0.0"\n'
        'source-root = "lib"\n\n'
        "[build]\n"
        'defines = ["PROJECT_FEATURE"]\n',
    )

    session = LspSession(executable)
    try:
        initialized = initialize(session, watched_files=True)
        legend = initialized["result"]["capabilities"]["semanticTokensProvider"][
            "legend"
        ]
        app_uri = open_document(session, app_path, app_source)
        initial_diagnostics = diagnostics_for(session, app_uri, version=1)
        assert initial_diagnostics == [], initial_diagnostics

        labels = completion_labels(session, 2, app_uri, app_source)
        assert {"value", "doubled"}.issubset(labels), labels

        value_use = app_source.index("value")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": app_uri},
                    "position": lsp_position(app_source, value_use + 1),
                },
            }
        )
        definition = session.receive_until(lambda message: message.get("id") == 3)[
            "result"
        ]
        assert definition["uri"] == dependency_path.resolve().as_uri(), definition

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": app_uri},
                    "position": lsp_position(app_source, value_use + 1),
                },
            }
        )
        hover = session.receive_until(lambda message: message.get("id") == 4)[
            "result"
        ]
        assert hover and "value" in json.dumps(hover), hover

        session.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": app_uri}},
            }
        )
        token_data = session.receive_until(lambda message: message.get("id") == 5)[
            "result"
        ]["data"]
        tokens = semantic_tokens_by_position(token_data)
        counter_position = lsp_position(app_source, app_source.index("Counter"))
        counter_token = tokens[
            (counter_position["line"], counter_position["character"])
        ]
        assert counter_token["type"] == legend["tokenTypes"].index("class"), (
            counter_token,
            legend,
        )

        dependency_uri = open_document(
            session, dependency_path, dependency_source, version=1
        )
        dependency_diagnostics = diagnostics_for(session, dependency_uri, version=1)
        assert all(
            item.get("code") == "GTI-S2075" for item in dependency_diagnostics
        ), dependency_diagnostics

        dirty_dependency = dependency_source.replace(
            "int value() { return 7; }", "int answer() { return 9; }"
        )
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": dependency_uri, "version": 2},
                    "contentChanges": [{"text": dirty_dependency}],
                },
            }
        )
        broken = diagnostics_for(
            session,
            app_uri,
            version=1,
            predicate=lambda items: any(
                item.get("code") == "GTI-S2000" or item.get("code") == "GTI-S2001"
                for item in items
            ),
        )
        assert any("value" in item["message"] for item in broken), broken

        updated_app = app_source.replace("counter.value()", "counter.answer()")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": app_uri, "version": 2},
                    "contentChanges": [{"text": updated_app}],
                },
            }
        )
        updated_diagnostics = diagnostics_for(session, app_uri, version=2)
        assert updated_diagnostics == [], updated_diagnostics
        labels = completion_labels(session, 6, app_uri, updated_app)
        assert "answer" in labels and "value" not in labels, labels

        answer_use = updated_app.index("answer")
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": app_uri},
                    "position": lsp_position(updated_app, answer_use + 1),
                },
            }
        )
        dirty_definition = session.receive_until(
            lambda message: message.get("id") == 7
        )["result"]
        assert dirty_definition["uri"] == dependency_uri, dirty_definition
    finally:
        session.close()


def test_dependency_warning_policy(executable, root):
    app = root / "warning-app"
    dependency = root / "warning-library"
    app_path = app / "src/main.gti"
    dependency_path = dependency / "src/value.gti"
    app_source = (
        "#include <library/value>\n"
        "int main() { mut int own = 0; return library::value() + own; }\n")
    dependency_source = (
        "namespace library { int value() { mut int value = 0; return value; } }\n")
    write(app_path, app_source)
    write(dependency_path, dependency_source)
    manifest_path = app / "gti.toml"
    base_manifest = (
        'manifest-version = 1\n[package]\nname = "warning_app"\n'
        'version = "1.0.0"\n[dependencies]\nlibrary = { path = "../warning-library" }\n'
        '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n')
    write(manifest_path, base_manifest)
    write(dependency / "gti.toml",
          'manifest-version = 1\n[package]\nname = "warning_library"\n'
          'version = "1.0.0"\n[profiles.dev]\ndependency-warnings = false\n')

    session = LspSession(executable)
    try:
        initialize(session, watched_files=True)
        app_uri = open_document(session, app_path, app_source)
        dependency_uri = dependency_path.resolve().as_uri()

        def publications_for_both():
            publications = {}

            def received(message):
                if message.get("method") == "textDocument/publishDiagnostics":
                    params = message["params"]
                    if params.get("uri") in (app_uri, dependency_uri):
                        publications[params["uri"]] = params["diagnostics"]
                return (len(publications) == 2 and any(
                    item.get("code") == "GTI-S2096"
                    for item in publications.get(app_uri, [])))

            session.receive_until(received)
            return publications

        initial = publications_for_both()
        assert [item.get("code") for item in initial[app_uri]] == ["GTI-S2096"], initial
        assert [item.get("code") for item in initial[dependency_uri]] == ["GTI-S2096"], initial

        quiet_profile = "\n[profiles.dev]\ndependency-warnings = false\n"

        def changed_manifest(text):
            write(manifest_path, text)
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [
                              {"uri": manifest_path.resolve().as_uri(), "type": 2}]}})

        changed_manifest(base_manifest + quiet_profile)
        suppressed = publications_for_both()
        assert [item.get("code") for item in suppressed[app_uri]] == ["GTI-S2096"], suppressed
        assert suppressed[dependency_uri] == [], suppressed
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
                      "params": {"textDocument": {"uri": app_uri},
                                 "position": lsp_position(app_source, app_source.index("value()") + 1)}})
        hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
        assert hover and "value" in json.dumps(hover), hover

        changed_manifest(base_manifest + quiet_profile + 'disabled-warnings = ["unused-mut"]\n')
        grouped = diagnostics_for(session, app_uri, version=1,
                                  predicate=lambda items: not items)
        assert grouped == [], grouped
        changed_manifest(base_manifest + quiet_profile)
        retained = diagnostics_for(session, app_uri, version=1,
                                   predicate=lambda items: any(
                                       item.get("code") == "GTI-S2096" for item in items))
        assert len(retained) == 1, retained

        write(dependency_path,
              "namespace library { int value() { return missing; } }\n")
        session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                      "params": {"changes": [{"uri": dependency_uri, "type": 2}]}})
        error = diagnostics_for(session, dependency_uri,
                                predicate=lambda items: any(
                                    item.get("severity") == 1 for item in items))
        assert any(item.get("code") == "GTI-S2001" for item in error), error
        write(dependency_path, dependency_source)
        session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                      "params": {"changes": [{"uri": dependency_uri, "type": 2}]}})
        recovered = publications_for_both()
        assert recovered[dependency_uri] == [], recovered
        assert [item.get("code") for item in recovered[app_uri]] == ["GTI-S2096"], recovered

        # A dependency opened as its own project is now the selected package;
        # its own warnings must remain visible even though the app suppressed it.
        open_document(session, dependency_path, dependency_source)
        selected = diagnostics_for(session, dependency_uri, version=1,
                                   predicate=lambda items: any(
                                       item.get("code") == "GTI-S2096" for item in items))
        assert [item.get("code") for item in selected] == ["GTI-S2096"], selected
        assert not (app / "build").exists(), "warning policy should not build state"
    finally:
        session.close()


def test_read_only_git_failure(executable, root):
    project = root / "locked-app"
    source_path = project / "src/main.gti"
    source = "#include <missing/unit>\nint main() { return 0; }\n"
    write(source_path, source)
    manifest_path = project / "gti.toml"
    write(
        manifest_path,
        "manifest-version = 1\n\n"
        "[package]\n"
        'name = "locked_app"\n'
        'version = "0.1.0"\n\n'
        "[dependencies]\n"
        'missing = { git = "https://example.invalid/missing.git", '
        'rev = "0000000000000000000000000000000000000000" }\n\n'
        "[targets.locked_app]\n"
        'kind = "executable"\n'
        'root = "src/main.gti"\n',
    )

    session = LspSession(executable)
    try:
        initialize(session)
        open_document(session, source_path, source)
        manifest_diagnostics = diagnostics_for(
            session,
            manifest_path.resolve().as_uri(),
            predicate=lambda items: any(
                item.get("code") == "GTI-B1701" for item in items
            ),
        )
        assert any(item.get("code") == "GTI-B1701" for item in manifest_diagnostics)
        assert not (project / "build").exists(), "LSP project resolution built state"
        assert not (project / "gti.lock").exists(), "LSP project resolution fetched"

        lock_path = project / "gti.lock"
        write(lock_path, "not valid toml [\n")
        session.send(
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWatchedFiles",
                "params": {
                    "changes": [
                        {"uri": lock_path.resolve().as_uri(), "type": 2}
                    ]
                },
            }
        )
        lock_diagnostics = diagnostics_for(
            session,
            lock_path.resolve().as_uri(),
            predicate=lambda items: any(
                item.get("code") == "GTI-B1702" for item in items
            ),
        )
        assert any(item.get("code") == "GTI-B1702" for item in lock_diagnostics)
        assert not (project / "build").exists(), "lock refresh built project state"
    finally:
        session.close()


def test_standalone_compatibility(executable, root):
    source_path = root / "standalone.gti"
    source = "int standalone_value() { return 1; }\n"
    write(source_path, source)
    session = LspSession(executable)
    try:
        initialize(session)
        uri = open_document(session, source_path, source)
        standalone_diagnostics = diagnostics_for(session, uri, version=1)
        assert standalone_diagnostics == [], standalone_diagnostics
    finally:
        session.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_project_facts_test.py /path/to/gti_lsp")
    with tempfile.TemporaryDirectory(prefix="gti-lsp-project-facts-") as temp:
        root = pathlib.Path(temp)
        test_path_dependency_tooling(sys.argv[1], root)
        test_dependency_warning_policy(sys.argv[1], root)
        test_read_only_git_failure(sys.argv[1], root)
        test_standalone_compatibility(sys.argv[1], root)


if __name__ == "__main__":
    main()
