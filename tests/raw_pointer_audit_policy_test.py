#!/usr/bin/env python3
"""One compiler policy across direct/project/child builds and LSP snapshots."""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile

from lsp_project_facts_test import (
    AUDIT_TEST_ENVIRONMENT,
    audited_diagnostics,
    diagnostics_for,
    initialize,
    open_document,
)
from lsp_smoke_test import LspSession, lsp_position


SOURCE = """int main() {
  mut const int* pointer = nullptr;
  { int value = 7; pointer = &value; }
  unsafe { /*😀*/ return *pointer; }
}
"""
SAFE = "int main() { return 0; }\n"


def run(command, cwd, status=0):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    assert result.returncode == status, (command, result.returncode, result.stdout, result.stderr)
    return result


LIBRARY_SOURCE = "import std.memory;\nint main() { return 0; }\n"


def library_findings(stderr):
    """Audit findings whose location is inside the standard library."""
    return [line for line in stderr.splitlines()
            if "stdlib/std/" in line and "GTI-S210" in line]


def manifest(policy, library_findings=None):
    opt_in = ("" if library_findings is None else
              "raw-pointer-audit-library-findings = "
              f"{'true' if library_findings else 'false'}\n")
    return ('manifest-version = 1\n[package]\nname = "audit"\nversion = "1.0.0"\n'
            '[targets.audit]\nkind = "executable"\nroot = "main.gti"\n'
            '[targets.other]\nkind = "test"\nroot = "main.gti"\n'
            f'[profiles.dev]\nraw-pointer-audit = "{policy}"\n' + opt_in)


def lsp_check(lsp, source, args, expected_severity, watched=False):
    session = LspSession(str(lsp), args, environment=AUDIT_TEST_ENVIRONMENT)
    # With the audit enabled, each version publishes the edit-path result and
    # then the background audit's additions.
    audit = expected_severity is not None
    try:
        initialize(session, watched_files=watched)
        uri = open_document(session, source, SOURCE)
        diagnostics = audited_diagnostics(session, uri, 1, audit)
        findings = [d for d in diagnostics if d.get("code") == "GTI-S2101"]
        if expected_severity is None:
            assert not findings and not [d for d in diagnostics if d.get("severity") == 1], diagnostics
        else:
            assert len(findings) == 1 and findings[0]["severity"] == expected_severity, diagnostics
            assert findings[0]["range"]["start"] == lsp_position(SOURCE, SOURCE.index("*pointer")), findings
        if watched:
            # Policy changes must invalidate a snapshot even with identical source.
            (source.parent / "gti.toml").write_text(manifest("off"))
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": (source.parent / "gti.toml").as_uri(), "type": 2}]}})
            # A publication already queued before the watcher notification can
            # retain the same document version. Wait for the refreshed policy.
            updated = diagnostics_for(session, uri, 1, predicate=lambda items:
                not any(d.get("code", "").startswith("GTI-S210") for d in items))
            assert not [d for d in updated if d.get("code", "").startswith("GTI-S210")], updated
            audit = False
        # Unsaved repaired source, incomplete recovery, and close/reopen use fresh generations.
        for version, text in ((2, SAFE), (3, "int main( {"), (4, SAFE)):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})
            updated = audited_diagnostics(session, uri, version, audit)
            assert bool([d for d in updated if d.get("severity") == 1]) == (version == 3), updated
        session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
        diagnostics_for(session, uri)
        open_document(session, source, SAFE, 5)
        assert not [d for d in audited_diagnostics(session, uri, 5, audit)
                    if d.get("severity") == 1]
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=pathlib.Path)
    parser.add_argument("lsp", type=pathlib.Path)
    parser.add_argument("--installed", action="store_true",
                        help="Exercise the installed library layout without a source-tree override")
    arguments = parser.parse_args()
    gti, lsp = arguments.gti.resolve(), arguments.lsp.resolve()
    if arguments.installed:
        os.environ.pop("GTI_STDLIB_PATH", None)
    else:
        os.environ["GTI_STDLIB_PATH"] = str(pathlib.Path(__file__).resolve().parents[1] / "stdlib")
    with tempfile.TemporaryDirectory(prefix="gti-raw-audit-policy-") as directory:
        root = pathlib.Path(directory)
        source = root / "main.gti"
        source.write_text(SOURCE)
        for mode, status in ((None, 0), ("off", 0), ("warn", 0), ("strict", 65)):
            command = [str(gti), str(source), "--emit-cpp", "-o", str(root / "out.cpp")]
            if mode:
                command.append(f"--raw-pointer-audit={mode}")
            result = run(command, root, status)
            assert ("GTI-S2101" in result.stderr) == (mode in ("warn", "strict")), result.stderr
            if mode in ("warn", "strict"):
                assert ("warning[GTI-S2101]" if mode == "warn" else "error[GTI-S2101]") in result.stderr
        # Findings located inside the standard library are hidden from an
        # application by default; the flag opts in, and Strict then promotes them.
        library = root / "library.gti"
        library.write_text(LIBRARY_SOURCE)
        library_command = [str(gti), str(library), "--emit-cpp", "-o", str(root / "library.cpp")]
        hidden = run([*library_command, "--raw-pointer-audit=warn"], root)
        assert not library_findings(hidden.stderr), hidden.stderr
        shown = run([*library_command, "--raw-pointer-audit=warn",
                     "--raw-pointer-audit-library-findings"], root)
        assert library_findings(shown.stderr), shown.stderr
        promoted = run([*library_command, "--raw-pointer-audit=strict",
                        "--raw-pointer-audit-library-findings"], root, 65)
        assert any("error[GTI-S210" in line for line in library_findings(promoted.stderr)), promoted.stderr
        run([*library_command, "--raw-pointer-audit=strict"], root)
        run([*library_command, "--raw-pointer-audit-library-findings",
             "--no-raw-pointer-audit-library-findings"], root, 64)
        # Promoted strict findings are errors, not suppressible advisory warnings.
        run([str(gti), str(source), "--emit-cpp", "--raw-pointer-audit=strict", "-Wno-raw-pointer-violation"], root, 65)
        for args in (["--raw-pointer-audit=bad"], ["--raw-pointer-audit"],
                     ["--raw-pointer-audit=off", "--raw-pointer-audit=warn"]):
            run([str(gti), str(source), *args], root, 64)
        for mode, severity in ((None, None), ("warn", 2), ("strict", 1)):
            lsp_check(lsp, source, [] if mode is None else [f"--raw-pointer-audit={mode}"], severity)
        config = root / "gti.toml"
        for mode, status in (("off", 0), ("warn", 0), ("strict", 65)):
            config.write_text(manifest(mode))
            result = run([str(gti), "check", "audit"], root, status)
            assert ("GTI-S2101" in result.stderr) == (mode != "off"), result.stderr
        # The profile key opts an application into library-located findings.
        library_project = root / "library-project"
        library_project.mkdir()
        (library_project / "main.gti").write_text(LIBRARY_SOURCE)
        for opt_in, mode, status in ((None, "warn", 0), (False, "strict", 0),
                                     (True, "warn", 0), (True, "strict", 65)):
            (library_project / "gti.toml").write_text(manifest(mode, opt_in))
            result = run([str(gti), "check", "audit"], library_project, status)
            assert bool(library_findings(result.stderr)) == bool(opt_in), result.stderr
        (library_project / "gti.toml").write_text(manifest("warn", True))
        overridden = run([str(gti), "check", "audit",
                          "--no-raw-pointer-audit-library-findings"], library_project)
        assert not library_findings(overridden.stderr), overridden.stderr
        (library_project / "gti.toml").write_text(
            manifest("warn").replace('raw-pointer-audit = "warn"\n',
                                     'raw-pointer-audit = "warn"\n'
                                     'raw-pointer-audit-library-findings = "yes"\n'))
        malformed = run([str(gti), "check", "audit"], library_project, 65)
        assert "GTI-B1004" in malformed.stderr, malformed.stderr
        run([str(gti), "check", "audit", "--raw-pointer-audit=off"], root)
        config.write_text(manifest("off"))
        run([str(gti), "build", "--all", "--jobs", "2", "--raw-pointer-audit=strict"], root, 65)
        run([str(gti), "run", "audit", "--no-build", "--raw-pointer-audit=off"], root, 64)
        config.write_text(manifest("warn"))
        metadata = json.loads(run([str(gti), "metadata", "--format", "json"], root).stdout)
        assert next(p for p in metadata["profiles"] if p["name"] == "dev")["rawPointerAudit"] == "warn", metadata
        lsp_check(lsp, source, ["--raw-pointer-audit=off"], None)
        lsp_check(lsp, source, [], 2, watched=True)
    print("raw pointer audit direct/project/LSP policy passed")


if __name__ == "__main__":
    main()
