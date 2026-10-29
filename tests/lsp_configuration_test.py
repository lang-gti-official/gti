#!/usr/bin/env python3
"""Editor profile selection and inactive semantic-token protocol regression."""

import pathlib
import sys
import tempfile

from lsp_project_facts_test import (
    completion_labels, diagnostics_for, initialize, open_document, write,
)
from lsp_smoke_test import has_only_include_advice, LspSession, lsp_position, semantic_tokens_by_position


def test_profiles(executable, root):
    project = root / "editor-profiles"
    manifest_path = project / "gti.toml"
    manifest = (
        'manifest-version = 1\n[package]\nname = "editor"\nversion = "0.1.0"\n'
        '[targets.editor]\nkind = "executable"\nroot = "src/main.gti"\n'
        'defines = ["TARGET_FEATURE"]\n'
        '[profiles.dev]\ndefines = ["PSYCH_DEBUG"]\n'
        '[profiles.release]\ndefines = []\n'
        '[profiles.analysis]\ndefines = ["PSYCH_DEBUG"]\n'
    )
    source = (
        '#include "core/service.gti"\n'
        '#ifdef PSYCH_DEBUG\n'
        '/* 😀 */ int debug_branch() { return 0; }\n'
        '#else\n'
        '/* 😀 */ int release_branch() { return release_missing; }\n'
        '#endif\n'
        'int main() { Counter counter{}; return counter.value(); }\n'
    )
    dependency = (
        '#ifdef TARGET_FEATURE\n#ifdef PSYCH_DEBUG\n'
        'class Counter { public: Counter() = default; int value() { return 0; } '
        'int dev_only() { return 1; } };\n'
        '#else\n'
        'class Counter { public: Counter() = default; int value() { return 0; } '
        'int release_only() { return 2; } };\n'
        '#endif\n#endif\n'
    )
    path = project / "src/main.gti"
    dependency_path = project / "src/core/service.gti"
    write(manifest_path, manifest)
    write(path, source)
    write(dependency_path, dependency)
    session = LspSession(executable)
    try:
        initial = initialize(session, watched_files=True)
        legend = initial["result"]["capabilities"]["semanticTokensProvider"]["legend"]
        inactive_mask = 1 << legend["tokenModifiers"].index("inactiveCode")
        uri = open_document(session, path, source)
        assert has_only_include_advice(diagnostics_for(session, uri, version=1))
        next_id = 2

        def tokens(document_uri=uri):
            nonlocal next_id
            request_id = next_id
            next_id += 1
            session.send({
                "jsonrpc": "2.0", "id": request_id,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": document_uri}},
            })
            result = session.receive_until(lambda m: m.get("id") == request_id)
            return semantic_tokens_by_position(result["result"]["data"])

        def assert_profile(debug):
            result = tokens()
            for name, inactive in (("debug_branch", not debug), ("release_branch", debug)):
                offset = source.index(name)
                line_end = source.index("\n", offset)
                offsets = (offset, source.index("{", offset),
                           source.index(";", offset), line_end - 1)
                for token_offset in offsets:
                    p = lsp_position(source, token_offset)
                    token = result.get((p["line"], p["character"]))
                    if inactive:
                        assert token and token["modifiers"] & inactive_mask, (name, p, token)
                    else:
                        assert not token or not token["modifiers"] & inactive_mask, token

        def select(profile):
            session.send({
                "jsonrpc": "2.0", "method": "workspace/didChangeConfiguration",
                "params": {"settings": {"gti": {"profile": profile}}},
            })

        def labels():
            nonlocal next_id
            request_id = next_id
            next_id += 1
            return completion_labels(session, request_id, uri, source)

        assert_profile(True)
        assert "dev_only" in labels() and "release_only" not in labels()
        select("release")
        errors = diagnostics_for(session, uri, predicate=lambda items: any(
            d.get("code") == "GTI-S2001" for d in items))
        assert any("release_missing" in d["message"] for d in errors)
        assert not any(d.get("code") == "GTI-S2075" for d in errors)
        assert_profile(False)
        assert "release_only" in labels() and "dev_only" not in labels()

        dirty = dependency.replace("release_only", "dirty_release")
        dependency_uri = open_document(session, dependency_path, dirty)
        assert diagnostics_for(session, dependency_uri, version=1) == []
        assert "dirty_release" in labels() and "release_only" not in labels()
        p = lsp_position(dirty, dirty.index("dev_only"))
        assert tokens(dependency_uri)[(p["line"], p["character"])]["modifiers"] & inactive_mask

        # A newer selection supersedes queued work and preserves dirty overlays.
        select("dev")
        select("release")
        select("analysis")
        assert has_only_include_advice(diagnostics_for(session, uri, predicate=has_only_include_advice))
        assert_profile(True)
        assert "dev_only" in labels() and "dirty_release" not in labels()
        for invalid in (True, "", "../release"):
            select(invalid)
            error = session.receive_until(lambda m: m.get("method") == "window/showMessage")
            assert error["params"]["type"] == 1
            assert "previous selection is unchanged" in error["params"]["message"]
            assert_profile(True)

        # Unknown names must not fabricate default-profile semantic results.
        select("relase")
        errors = diagnostics_for(session, uri, predicate=lambda items: any(
            d.get("code") == "GTI-B1202" for d in items))
        error = next(d for d in errors if d.get("code") == "GTI-B1202")
        assert "relase" in error["message"] and "release" in error["message"]
        assert error["range"]["start"] == {"line": 0, "character": 0}
        assert error["range"]["end"] == {"line": 0, "character": 1}
        assert not any(t["modifiers"] & inactive_mask for t in tokens().values())
        assert not labels()
        select(None)
        assert_profile(True)
        assert "dev_only" in labels()

        # A selected profile still follows saved manifest edits.
        select("release")
        assert_profile(False)
        write(manifest_path, manifest.replace(
            '[profiles.release]\ndefines = []',
            '[profiles.release]\ndefines = ["PSYCH_DEBUG"]'))
        session.send({
            "jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
            "params": {"changes": [{"uri": manifest_path.resolve().as_uri(), "type": 2}]},
        })
        diagnostics_for(session, uri, predicate=has_only_include_advice)
        assert_profile(True)
        write(manifest_path, manifest)
        session.send({
            "jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
            "params": {"changes": [{"uri": manifest_path.resolve().as_uri(), "type": 2}]},
        })
        assert_profile(False)

        for document_uri in (dependency_uri, uri):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": document_uri}}})
        open_document(session, path, source, version=2)
        diagnostics_for(session, uri, version=2, predicate=lambda items: any(
            d.get("code") == "GTI-S2001" for d in items))
        assert_profile(False)
        assert "release_only" in labels() and "dirty_release" not in labels()
    finally:
        session.close()

    session = LspSession(executable)
    try:
        initialize(session, initialization_options={"profile": "release"})
        uri = open_document(session, path, source)
        errors = diagnostics_for(session, uri, version=1)
        assert any(d.get("code") == "GTI-S2001" for d in errors), errors
        assert "release_only" in completion_labels(session, 2, uri, source)
        # Profile selection is project-only; standalone fragments still work.
        standalone = root / "standalone.gti"
        write(standalone, "int value() { return 0; }\n")
        standalone_uri = open_document(session, standalone, standalone.read_text())
        assert diagnostics_for(session, standalone_uri, version=1) == []
    finally:
        session.close()


def test_selected_profile_policies(executable, root):
    project = root / "profile-policies"
    path = project / "main.gti"
    write(project / "gti.toml", (
        'manifest-version = 1\n[package]\nname = "policies"\nversion = "0.1.0"\n'
        '[targets.policies]\nkind = "executable"\nroot = "main.gti"\n'
        '[profiles.dev]\nexecution-profile = "single-threaded"\n'
        'raw-pointer-audit = "off"\ndisabled-warnings = ["unused-mut", "legacy-include"]\n'
        '[profiles.release]\nexecution-profile = "concurrent"\nraw-pointer-audit = "warn"\n'
    ))
    source = (
        '#include <std/thread>\n'
        'int warning() { mut int value = 0; return value; }\n'
        'int dangling() { mut const int* pointer = nullptr; '
        '{ int value = 7; pointer = &value; } unsafe { return *pointer; } }\n'
        'int main() {\n'
        '  auto started = std::thread::start([]() -> void {});\n'
        '  if (!started.has_value()) { return 1; }\n'
        '  mut std::thread worker = std::move(started).value();\n'
        '  auto joined = worker.join();\n'
        '  if (!joined.has_value()) { return 2; }\n'
        '  return warning();\n'
        '}\n'
    )
    write(path, source)
    session = LspSession(executable)
    try:
        initialize(session)
        uri = open_document(session, path, source)
        dev = diagnostics_for(session, uri, version=1)
        codes = {d.get("code") for d in dev}
        assert "GTI-S2084" in codes, dev
        assert not codes.intersection({"GTI-S2096", "GTI-S2101", "GTI-I0013"}), dev
        session.send({
            "jsonrpc": "2.0", "method": "workspace/didChangeConfiguration",
            "params": {"settings": {"gti": {"profile": "release"}}},
        })
        release = diagnostics_for(session, uri, predicate=lambda items:
            {"GTI-S2096", "GTI-S2101", "GTI-I0013"}.issubset({d.get("code") for d in items}))
        assert not any(d.get("severity") == 1 for d in release), release
        assert all(d["severity"] == 2 for d in release if d.get("code") == "GTI-S2101")
    finally:
        session.close()

    session = LspSession(executable, ["--execution-profile", "single-threaded",
                                      "--raw-pointer-audit=off"])
    try:
        initialize(session, initialization_options={"profile": "release"})
        uri = open_document(session, path, source)
        diagnostics = diagnostics_for(session, uri, version=1)
        codes = {d.get("code") for d in diagnostics}
        assert "GTI-S2084" in codes, diagnostics
        assert "GTI-S2101" not in codes, diagnostics
    finally:
        session.close()

    # Audit override remains effective even with otherwise valid analysis.
    session = LspSession(executable, ["--raw-pointer-audit=off"])
    try:
        initialize(session, initialization_options={"profile": "release"})
        uri = open_document(session, path, source)
        diagnostics = diagnostics_for(session, uri, version=1)
        codes = {d.get("code") for d in diagnostics}
        assert "GTI-S2096" in codes and "GTI-S2101" not in codes, diagnostics
        assert not any(d.get("severity") == 1 for d in diagnostics), diagnostics
    finally:
        session.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lsp_configuration_test.py /path/to/gti_lsp")
    with tempfile.TemporaryDirectory(prefix="gti-lsp-configuration-") as directory:
        test_profiles(sys.argv[1], pathlib.Path(directory))
        test_selected_profile_policies(sys.argv[1], pathlib.Path(directory))


if __name__ == "__main__":
    main()
