#!/usr/bin/env python3
"""The gti_test_ui gate: gti-test's own behaviour, then tidy and tests/ui.

usage: check.py <gti-test> [--jobs N] [--self-test-only]

The self-test drives gti-test over tests/runner/fixtures and checks every
outcome class it reports: pass, wrong/missing/unmatched annotations, stdout
mismatch, internal compiler error, crash, CPU and wall-clock limits, bless
(including its refusals), tidy, sharding, JSON and --isolate; and the other
test kinds: parse-only modes, analysis options, and the mir, llvm-ir,
diagnostics and format snapshots with their failure and bless paths.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/runner/fixtures"


def run(runner, *args, env=None, cwd=ROOT, fixtures=True):
    environment = dict(os.environ)
    environment.pop("CI", None)
    environment.pop("GTI_TEST_SELF_TEST", None)
    if fixtures:
        environment["GTI_TEST_SELF_TEST"] = "1"
    environment.update(env or {})
    result = subprocess.run([str(runner), *map(str, args)], cwd=cwd,
                            env=environment, capture_output=True, text=True,
                            timeout=600)
    return result.returncode, result.stdout + result.stderr


def expect(condition, message, output=""):
    if not condition:
        sys.stderr.write(f"FAIL gti-test self-test: {message}\n{output}\n")
        sys.exit(1)


def block(output, status, name):
    """The failure block for one test, or ''."""
    match = re.search(rf"^{status} \S*{re.escape(name)} .*?(?=^\S|\Z)", output,
                      re.M | re.S)
    return match.group(0) if match else ""


def self_test(runner):
    code, out = run(runner, FIXTURES / "pass", "--jobs", "2")
    expect(code == 0 and "RESULT ok: 6 passed, 0 failed" in out,
           "passing fixtures pass", out)

    code, out = run(runner, FIXTURES / "fail", "--jobs", "2")
    expect(code == 1 and "RESULT fail: 0 passed, 4 failed" in out,
           "failing fixtures fail", out)
    wrong = block(out, "FAIL", "wrong_code.gti")
    expect("expected: ERROR GTI-S9999 at line 3" in wrong and
           "actual:   ERROR GTI-S2001 at line 3" in wrong and
           "rerun: " in wrong and "--bless" in wrong,
           "wrong code shows expected, actual, rerun and bless", out)
    expect("actual:   ERROR GTI-S2001 at line 4" in
           block(out, "FAIL", "unannotated.gti"),
           "an unannotated error fails", out)
    unmatched = block(out, "FAIL", "unmatched.gti")
    expect("expected: ERROR GTI-S2001 at line 4" in unmatched and
           "actual:" not in unmatched, "an unmatched annotation fails", out)
    expect("-expected\n      +actual" in block(out, "FAIL", "wrong_stdout.gti"),
           "stdout mismatch shows a diff", out)

    faults = {"GTI_TEST_TIME_SCALE": "0.1"}
    code, out = run(runner, FIXTURES / "faults", "--jobs", "2", env=faults)
    expect(code == 1, "faults fail the run", out)
    expect("the worker died with SIGSEGV" in block(out, "CRASH", "crash.gti"),
           "a crash is attributed to its case", out)
    expect("never blessable" in block(out, "ICE", "ice.gti"),
           "an internal compiler error is reported", out)
    expect("exception escaped" in block(out, "ICE", "throw.gti"),
           "an escaping exception is an internal compiler error", out)
    expect("CPU limit" in block(out, "TIMEOUT", "spin.gti"),
           "a spinning case exceeds its CPU limit", out)
    expect("wall time" in block(out, "TIMEOUT", "hang.gti"),
           "a hanging case reaches the wall-clock backstop", out)
    expect(re.search(r"RESULT fail: 1 passed, 0 failed, 2 ice, 2 timeout, "
                     r"1 crash", out), "a restarted worker keeps running cases",
           out)

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "ui"
        shutil.copytree(FIXTURES / "fail", work)
        shutil.copy(FIXTURES / "faults/cases/ice.gti", work / "annotations")
        before = {path: path.read_text() for path in work.rglob("*.gti")}

        code, out = run(runner, work, "--bless", env={"CI": "true"})
        expect(code == 2 and "refused" in out, "bless is refused under CI", out)
        expect(all(path.read_text() == text for path, text in before.items()),
               "a refused bless changes nothing", out)

        code, out = run(runner, work, "--bless", "--jobs", "2")
        expect(code == 1 and "ICE" in out and out.count("BLESSED") == 3,
               "bless rewrites failures but not the internal error", out)
        ice = work / "annotations/ice.gti"
        expect(ice.read_text() == before[ice], "an ICE is never blessed", out)
        declared = work / "annotations/unannotated.gti"
        expect(declared.read_text() == before[declared] and
               "not blessed: a check-pass test" in out,
               "bless keeps a declared check-pass test failing", out)
        ice.unlink()
        declared.unlink()
        code, out = run(runner, work, "--jobs", "2")
        expect(code == 0, "blessed tests pass", out)
        wrong = (work / "annotations/wrong_code.gti").read_text()
        expect("//~ ERROR GTI-S2001" in wrong and "S9999" not in wrong,
               "bless rewrote the annotation", wrong)
        unmatched = (work / "annotations/unmatched.gti").read_text()
        expect(unmatched.count("//~") == 1, "bless dropped the stale annotation",
               unmatched)
        stdout = (work / "run/wrong_stdout.gti").read_text()
        expect("//@ stdout: actual" in stdout and "expected" not in stdout,
               "bless rewrote the inline stdout", stdout)

    kinds_self_test(runner)

    code, out = run(runner, "--tidy", "--root", FIXTURES / "tidy")
    for problem in ("unknown directive 'check-pas'", "no_purpose.gti: the first line",
                    "orphan.stdout: expected output without a test",
                    "top_level.gti: tests live in a subdirectory",
                    "orphan_snapshot.mir: expected file without `//@ snapshot: mir`",
                    "unblessed.gti: no unblessed.stderr",
                    "oversized.formatted: snapshot is 450 lines, over the 400-line "
                    "limit; narrow it",
                    "bad_check.gti: line 0: CHECK patterns need",
                    "bad_check_kind.gti: line 3: patterns are"):
        expect(code == 1 and problem in out, f"tidy reports {problem}", out)
    code, out = run(runner, "--tidy", "--root", FIXTURES / "pass")
    expect(code == 0, "tidy accepts the passing fixtures", out)

    _, full = run(runner, FIXTURES / "pass", "--list")
    _, first = run(runner, FIXTURES / "pass", "--list", "--shard", "1/2")
    _, second = run(runner, FIXTURES / "pass", "--list", "--shard", "2/2")
    expect(sorted(first.splitlines() + second.splitlines()) ==
           sorted(full.splitlines()) and len(full.splitlines()) == 6,
           "shards partition the tests", full + first + second)

    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"
        code, out = run(runner, FIXTURES / "pass", "--isolate", "--json", report)
        expect(code == 0 and "isolated" in out, "--isolate agrees", out)
        rows = [json.loads(line) for line in report.read_text().splitlines()]
        expect(len(rows) == 6 and all("cpu" in row for row in rows),
               "the JSON report has one row per test", report.read_text())


def kinds_self_test(runner):
    """Parse-only modes, analysis options and snapshot kinds."""
    for isolate in ((), ("--isolate",)):
        code, out = run(runner, FIXTURES / "kinds", "--jobs", "4", *isolate)
        expect(code == 0 and "RESULT ok: 11 passed, 0 failed" in out,
               f"the other test kinds pass {isolate}", out)
    code, out = run(runner, "--tidy", "--root", FIXTURES / "kinds")
    expect(code == 0, "tidy accepts the kind fixtures", out)

    code, out = run(runner, FIXTURES / "snapshot", "--jobs", "4")
    expect(code == 1 and "RESULT fail: 0 passed, 6 failed" in out,
           "drifted snapshots fail", out)
    mir = block(out, "FAIL", "mir_drift.gti")
    expect("snapshot mir differs from mir_drift.mir (-expected +actual)" in mir
           and "-stale" in mir and "+body kind=3" in mir,
           "a MIR snapshot mismatch shows a diff", out)
    expect("no function 'nosuch' has a MIR body" in
           block(out, "FAIL", "mir_unknown_function.gti"),
           "an unknown mir-function fails", out)
    expect("+main.gti:4:10: error[GTI-S2001]: Undefined name 'missing'." in
           block(out, "FAIL", "diagnostics_drift.gti"),
           "a rendered-diagnostics mismatch shows a diff", out)
    expect("no format_new.formatted yet; bless" in
           block(out, "FAIL", "format_new.gti"),
           "a missing snapshot file asks for a bless", out)
    expect("CHECK at line 4: `@llvm.smul.with.overflow` not found" in
           block(out, "FAIL", "llvm_missing.gti"),
           "a missing CHECK pattern fails", out)
    expect(re.search(r"CHECK-NOT at line 4: `@llvm.sadd.with.overflow` matched "
                     r"output line \d+", block(out, "FAIL", "llvm_not.gti")),
           "a matching CHECK-NOT fails", out)

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "snapshot"
        shutil.copytree(FIXTURES / "snapshot", work)
        code, out = run(runner, work, "--bless", "--jobs", "4")
        expect(code == 1 and out.count("BLESSED") == 3,
               "bless writes snapshot files but not CHECK patterns", out)
        expect((work / "drift/format_new.formatted").read_text() ==
               "// A format snapshot not blessed yet.\n//@ snapshot: format\n"
               "int main() {\n  return 0;\n}\n",
               "the format snapshot is the formatter's output",
               (work / "drift/format_new.formatted").read_text())
        code, out = run(runner, work, "--jobs", "4")
        expect("RESULT fail: 3 passed, 3 failed" in out,
               "blessed snapshots pass", out)


def main():
    runner = Path(sys.argv[1]).resolve()
    jobs = "4"
    if "--jobs" in sys.argv:
        jobs = sys.argv[sys.argv.index("--jobs") + 1]
    self_test(runner)
    print("gti-test self-test: ok", flush=True)
    if "--self-test-only" in sys.argv:
        return 0
    code, out = run(runner, "--tidy", "--root", ROOT / "tests/ui",
                    fixtures=False)
    sys.stdout.write(out)
    if code != 0:
        return code
    code, out = run(runner, "--root", ROOT / "tests/ui", "--jobs", jobs,
                    fixtures=False)
    sys.stdout.write(out)
    return code


if __name__ == "__main__":
    sys.exit(main())
