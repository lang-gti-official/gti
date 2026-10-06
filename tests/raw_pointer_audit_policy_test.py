#!/usr/bin/env python3
"""One compiler policy across direct/project/child builds and LSP snapshots."""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile

from lsp_project_facts_test import initialize, open_document, diagnostics_for
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


def manifest(policy):
    return ('manifest-version = 1\n[package]\nname = "audit"\nversion = "1.0.0"\n'
            '[targets.audit]\nkind = "executable"\nroot = "main.gti"\n'
            '[targets.other]\nkind = "test"\nroot = "main.gti"\n'
            f'[profiles.dev]\nraw-pointer-audit = "{policy}"\n')


def lsp_check(lsp, source, args, expected_severity, watched=False):
    session = LspSession(str(lsp), args)
    try:
        initialize(session, watched_files=watched)
        uri = open_document(session, source, SOURCE)
        diagnostics = diagnostics_for(session, uri, 1)
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
        # Unsaved repaired source, incomplete recovery, and close/reopen use fresh generations.
        for version, text in ((2, SAFE), (3, "int main( {"), (4, SAFE)):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})
            updated = diagnostics_for(session, uri, version)
            assert bool([d for d in updated if d.get("severity") == 1]) == (version == 3), updated
        session.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
        diagnostics_for(session, uri)
        open_document(session, source, SAFE, 5)
        assert not [d for d in diagnostics_for(session, uri, 5) if d.get("severity") == 1]
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
