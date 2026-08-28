#!/usr/bin/env python3

"""Focused tests for the decline-census parser and measurement boundary."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


FAILURES: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"ok   {label}")
        return
    FAILURES.append(label)
    print(f"FAIL {label}" + (f"\n     {detail}" if detail else ""))


def load_census(root: Path):
    path = root / "scripts/debug/decline_census.py"
    spec = importlib.util.spec_from_file_location("decline_census", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


MIR = """mir-v1 valid=1
body kind=4 owner=7
 places 1
  p1 root=0 projections=[]
 blocks 1
  bb1
    i1 assign destination=1 success-result-drop=- lifecycle=[]
end-body
body kind=3 owner=9
 places 1
  p1 root=5 projections=[field=1]
 blocks 1
  bb1
    i1 assign destination=1 success-result-drop=4 lifecycle=[event(kind=3)]
    i2 call destination=- success-result-drop=- lifecycle=[]
end-body
"""


STUB_COMPILER = r'''#!/usr/bin/env python3
from pathlib import Path
import sys

arguments = sys.argv[1:]
source_path = Path(next(item for item in arguments if item.endswith(".gti")))
source = source_path.read_text(encoding="utf-8")
output = Path(arguments[arguments.index("-o") + 1])
mir = """mir-v1 valid=1
body kind=4 owner=7
 places 1
  p1 root=0 projections=[]
 blocks 1
  bb1
    i1 assign destination=1 success-result-drop=- lifecycle=[]
end-body
body kind=3 owner=9
 places 1
  p1 root=5 projections=[field=1]
 blocks 1
  bb1
    i1 assign destination=1 success-result-drop=4 lifecycle=[event(kind=3)]
    i2 call destination=- success-result-drop=- lifecycle=[]
end-body
"""
if "--emit-cpp" in arguments:
    if "DECLINE" in source:
        print("error[GTI-B0001]: C++ emitter requires a coherent "
              "representation snapshot: verified MIR body is outside the "
              "complete C++ text vocabulary for 'probe' (body-kind 3 owner 9)",
              file=sys.stderr)
        raise SystemExit(1)
    output.write_text("// emitted\n", encoding="utf-8")
    raise SystemExit(0)
if "--emit-mir" in arguments:
    output.write_text(mir, encoding="utf-8")
    raise SystemExit(0)
raise SystemExit(9)
'''


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <repo-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    census = load_census(root)
    mir_lines = MIR.splitlines()

    bodies = census.body_segments(mir_lines)
    check([(kind, owner) for kind, owner, _segment in bodies]
          == [(4, 7), (3, 9)],
          "body parser retains exact kind and owner")
    exact = census.body_segment(mir_lines, 3, 9)
    check(exact and exact[0].startswith("body kind=3 owner=9")
          and exact[-1] == "end-body",
          "diagnostic body selection is exact")

    features = census.spellable_body_features(mir_lines)
    check(features == {
        "Constructor": {"assign:dest=Binding"},
        "Function": {
            "assign:dest=Loan+proj:staged:Replace",
            "call",
        },
    }, "place IDs and features stay body-local", repr(features))

    emitted = census.Outcome(
        program="emitted", kind="ok",
        spellable_by_kind={
            "Constructor": {"assign:dest=Binding"},
            "Function": {"call"},
        })
    declined = census.Outcome(
        program="declined", kind="decline", body_kind="Function",
        features={"assign:dest=Binding", "call"})
    census.derive_signatures([emitted, declined], False)
    check(declined.signature == ("assign:dest=Binding",),
          "spellability is scoped to the declining body kind",
          repr(declined.signature))

    corpus = census.Outcome(
        program="corpus", origin="corpus", kind="ok",
        spellable_by_kind={"Function": {"assign:dest=Binding"}})
    census.derive_signatures([emitted, corpus, declined], False)
    check(declined.signature == ("assign:dest=Binding",),
          "corpus evidence is opt-in")
    census.derive_signatures([emitted, corpus, declined], True)
    check(declined.signature[:1] == (
        "(every feature is spellable alone; the combination is not)",),
        "opted-in corpus evidence participates in subtraction",
        repr(declined.signature))

    generated = census.generated_programs()
    regenerated = census.generated_programs()
    check(len(generated) == 36
          and len({program.name for program in generated}) == 36
          and [(program.name, program.source) for program in generated]
          == [(program.name, program.source) for program in regenerated],
          "generated shape sweep is complete and deterministic")

    with tempfile.TemporaryDirectory(prefix="gti-decline-unit-") as raw:
        temp = Path(raw)
        stub = temp / "stub-gti"
        stub.write_text(STUB_COMPILER, encoding="utf-8")
        stub.chmod(0o755)

        accepted = census.classify(
            stub, census.Program("accepted", "int main() { return 0; }\n"),
            temp, 1)
        check(accepted.kind == "ok"
              and accepted.spellable_by_kind == features,
              "classification measures backend emission without native C++",
              repr(accepted))

        decline = census.classify(
            stub, census.Program("decline", "DECLINE\n"), temp, 2)
        check(decline.kind == "decline"
              and decline.body_kind == "Function"
              and decline.features == features["Function"],
              "decline diagnostic selects the named verified MIR body",
              repr(decline))

        script = root / "scripts/debug/decline_census.py"
        missing_baseline = subprocess.run(
            [sys.executable, str(script), "--update"],
            capture_output=True, text=True, timeout=30, check=False)
        check(missing_baseline.returncode == 2
              and "--update requires --baseline" in missing_baseline.stderr,
              "baseline updates require an explicit target",
              missing_baseline.stdout + missing_baseline.stderr)

        missing_corpus = subprocess.run(
            [sys.executable, str(script), "--compiler", str(stub),
             "--corpus", str(temp / "missing")],
            capture_output=True, text=True, timeout=30, check=False)
        check(missing_corpus.returncode == 2
              and "corpus directory not found" in missing_corpus.stderr,
              "missing corpus roots fail instead of silently contributing "
              "nothing",
              missing_corpus.stdout + missing_corpus.stderr)

        broken = temp / "broken-gti"
        broken.write_text("#!/bin/sh\nexit 9\n", encoding="utf-8")
        broken.chmod(0o755)
        incomplete_baseline = temp / "incomplete.json"
        incomplete = subprocess.run(
            [sys.executable, str(script), "--compiler", str(broken),
             "--baseline", str(incomplete_baseline), "--update"],
            capture_output=True, text=True, timeout=30, check=False)
        check(incomplete.returncode == 1
              and "refusing to update" in incomplete.stdout
              and not incomplete_baseline.exists(),
              "incomplete measurements cannot become a baseline",
              incomplete.stdout + incomplete.stderr)

    if FAILURES:
        print(f"\n{len(FAILURES)} decline-census check(s) failed",
              file=sys.stderr)
        return 1
    print("\nall decline-census checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
