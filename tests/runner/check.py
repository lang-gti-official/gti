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
from xml.etree import ElementTree
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/runner/fixtures"


LAST_RUN = Path(tempfile.mkdtemp(prefix="gti-test-self-")) / "last-run"


def run(runner, *args, env=None, cwd=ROOT, fixtures=True):
    environment = dict(os.environ)
    environment.pop("CI", None)
    environment.pop("GTI_TEST_SELF_TEST", None)
    # The self-test's runs keep their own --failed record; the real run's
    # stays beside the runner for `gti-test --failed`.
    if fixtures:
        environment["GTI_TEST_LAST_RUN"] = str(LAST_RUN)
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
    expect(code == 0 and "RESULT ok: 8 passed, 0 failed" in out,
           "passing fixtures pass", out)

    code, out = run(runner, FIXTURES / "fail", "--jobs", "2")
    expect(code == 1 and "RESULT fail: 0 passed, 5 failed" in out,
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
    revision = block(out, "FAIL", "wrong_revision.gti")
    expect("[bad] stdout differs" in revision and "[good]" not in revision,
           "a failing revision is named and a passing one is not", out)

    faults = {"GTI_TEST_TIME_SCALE": "0.1"}
    code, out = run(runner, FIXTURES / "faults", "--jobs", "2", env=faults)
    expect(code == 1, "faults fail the run", out)
    # A sanitized runner's handler reports the SIGSEGV and then aborts.
    crash = block(out, "CRASH", "crash.gti")
    expect("the worker died with SIGSEGV" in crash or
           "the worker died with SIGABRT" in crash,
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

        stdout_test = work / "run/wrong_stdout.gti"
        code, out = run(runner, stdout_test, "--bless")
        expect(code == 1 and "not blessed: the expected output is `//@ stdout:`"
               in out and stdout_test.read_text() == before[stdout_test],
               "bless leaves `//@` lines alone without --bless-directives", out)
        code, out = run(runner, work, "--bless", "--bless-directives", "--jobs",
                        "2")
        expect(code == 1 and "ICE" in out and out.count("BLESSED") == 4,
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
        revision = (work / "run/wrong_revision.gti").read_text()
        expect("//@[bad] stdout: actual" in revision and
               "expected" not in revision,
               "bless rewrote the revision's inline stdout", revision)

    code, out = run(runner, FIXTURES / "format")
    expect(code == 1 and "is not idempotent" in block(out, "FAIL", "drift.gti"),
           "format-stable fails when formatting is not a fixed point", out)
    kinds_self_test(runner)
    ergonomics_self_test(runner)
    reporting_self_test(runner)
    project_self_test(runner)
    if (runner.parent / "gti_lsp").exists():
        lsp_self_test(runner)

    code, out = run(runner, "--tidy", "--root", FIXTURES / "tidy")
    for problem in ("unknown directive 'check-pas'", "no_purpose.gti: the first line",
                    "format-stable needs a check-pass test without annotations",
                    "orphan.stdout: expected output without a test",
                    "top_level.gti: tests live in a subdirectory",
                    "orphan_snapshot.mir: expected file without `//@ snapshot: mir`",
                    "unblessed.gti: no unblessed.stderr",
                    "oversized.formatted: snapshot is 450 lines, over the 400-line "
                    "limit; narrow it",
                    "bad_check.gti: line 0: CHECK patterns need",
                    "bad_check_kind.gti: line 3: patterns are",
                    "revision 'third' is not declared",
                    "stray_revision.fourth.stdout: expected output without a test",
                    "project/test.transcript: line 2: expectations follow",
                    "project/test.transcript: line 4: exit codes are",
                    "bad_overlay.gti: line 0: stdlib-overlay names "
                    "'missing.gti', which is neither a section nor",
                    "native_check.gti: line 0: C/C++ sections and native-args "
                    "need build-pass",
                    "reserved_native_args.gti: line 0: native-args cannot pass '-o'",
                    "signal_run_pass.gti: line 4: configuration flag '9bad'",
                    "signal_run_pass.gti: line 5: env is written",
                    "signal_run_pass.gti: line 0: exit-signal needs run-fail",
                    "allow_warnings.gti: line 2: allow-warnings is only for a "
                    "gti-test.defaults file",
                    "tidy/bad/gti-test.defaults: defaults files are for "
                    "directive-less roots",
                    "bad_option.jsonl: line 2: a transcript takes only "
                    "`//@ execution-profile:` and `//@ raw-pointer-audit:`",
                    "bad_sections.gti: line 3: execution-profile must be",
                    "bad_sections.gti: line 0: `step 2 other.gti` edits "
                    "'other.gti', which is not a .gti file section",
                    "bad_sections.gti: line 0: disk-only names 'missing.gti'",
                    "bad_sections.gti: line 9: annotations belong in opened "
                    ".gti documents",
                    "bad_args.gti: line 3: args has an unterminated quote",
                    "new_directives.gti: line 5: 'constexpr-depth' must be an "
                    "integer from 1",
                    "new_directives.gti: line 6: target 'nonsense' is not a "
                    "supported triple",
                    "new_directives.gti: line 0: analyze tests stop before "
                    "HIR"):
        expect(code == 1 and problem in out, f"tidy reports {problem}", out)
    code, out = run(runner, "--tidy", "--root", FIXTURES / "pass")
    expect(code == 0, "tidy accepts the passing fixtures", out)

    _, full = run(runner, FIXTURES / "pass", "--list")
    _, first = run(runner, FIXTURES / "pass", "--list", "--shard", "1/2")
    _, second = run(runner, FIXTURES / "pass", "--list", "--shard", "2/2")
    expect(sorted(first.splitlines() + second.splitlines()) ==
           sorted(full.splitlines()) and len(full.splitlines()) == 8,
           "shards partition the tests", full + first + second)

    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"
        code, out = run(runner, FIXTURES / "pass", "--isolate", "--json", report)
        expect(code == 0 and "isolated (0 in-process" in out,
               "--isolate agrees and counts in-process cases", out)
        rows = [json.loads(line) for line in report.read_text().splitlines()]
        expect(len(rows) == 8 and all("cpu" in row for row in rows),
               "the JSON report has one row per test", report.read_text())


def reporting_self_test(runner):
    """--opt, --filter, --slowest, --cpu-budget, argument checks, one-pass
    bless of rendered diagnostics."""
    code, out = run(runner, FIXTURES / "opt")
    expect(code == 1 and "CHECK-NOT at line 6: `alloca`" in
           block(out, "FAIL", "follows.gti"),
           "without the override a run test builds unoptimized", out)
    for override in ((("--opt", "O3"), {}), ((), {"GTI_TEST_OPT": "O3"})):
        code, out = run(runner, FIXTURES / "opt", *override[0], env=override[1])
        expect(code == 0 and "RESULT ok: 2 passed" in out,
               f"--opt O3 / GTI_TEST_OPT reach unpinned run tests {override}",
               out)
    code, out = run(runner, FIXTURES / "opt", "--opt", "O2")
    expect(code == 2 and "is O0 or O3" in out, "--opt rejects other levels", out)

    code, out = run(runner, FIXTURES / "pass", "--filter", r"run_\w+\.gti$",
                    "--list")
    expect(code == 0 and sorted(line.split()[0].rsplit("/", 1)[1]
                                for line in out.splitlines()) ==
           ["run_exit.gti", "run_output.gti"], "--filter selects by regex", out)
    code, out = run(runner, FIXTURES / "pass", "--filter", "(")
    expect(code == 2 and "--filter" in out, "a bad regex is a usage error", out)
    code, out = run(runner, FIXTURES / "pass", "--slowest", "3")
    expect(code == 0 and out.count("\nSLOW ") == 3 and "BUDGET" not in out,
           "--slowest lists the most expensive tests", out)
    code, out = run(runner, FIXTURES / "pass", "--cpu-budget", "1000")
    expect(code == 0 and re.search(r"BUDGET ok: [\d.]+ of 1000.0 cpu-s", out),
           "a run within its CPU budget passes and says so", out)
    code, out = run(runner, FIXTURES / "pass", env={"GTI_TEST_CPU_BUDGET":
                                                    "0.001"})
    expect(code == 1 and "BUDGET fail:" in out and out.count("\nSLOW ") == 8
           and "RESULT ok" in out,
           "a run over its CPU budget fails, listing the slowest tests", out)

    code, out = run(runner, "tests/ui/a\nb")
    expect(code == 2 and "holds a line break" in out and "Traceback" not in out,
           "an argument with a line break is a usage error", out)

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "ui"
        (work / "x").mkdir(parents=True)
        test = work / "x/rendered.gti"
        test.write_text("// Rendered diagnostics bless with the annotations.\n"
                        "//@ snapshot: diagnostics\n"
                        "int main() {\n  return missing;\n}\n")
        code, out = run(runner, test, "--bless")
        expect(code == 0 and out.count("BLESSED") == 2 and
               "//~ ERROR GTI-S2001" in (work / "x/rendered.stderr").read_text(),
               "one bless writes the annotation and the .stderr quoting it", out)
        code, out = run(runner, test)
        expect(code == 0, "the once-blessed test passes", out)


def ergonomics_self_test(runner):
    """--failed, --junit, --changed and its budget, default roots, strays."""
    run(runner, FIXTURES / "fail", "--jobs", "2")
    code, out = run(runner, FIXTURES / "pass", FIXTURES / "fail", "--failed",
                    "--jobs", "2")
    expect(code == 1 and "RESULT fail: 0 passed, 5 failed" in out,
           "--failed re-runs exactly the last run's failures", out)
    run(runner, FIXTURES / "pass", "--jobs", "2")
    code, out = run(runner, FIXTURES / "pass", "--failed")
    expect(code == 0 and "no failed tests" in out,
           "--failed after a clean run runs nothing", out)

    with tempfile.TemporaryDirectory() as directory:
        junit = Path(directory) / "junit.xml"
        run(runner, FIXTURES / "fail", "--jobs", "2", "--junit", junit)
        suite = ElementTree.parse(junit).getroot().find("testsuite")
        cases = suite.findall("testcase")
        expect(suite.get("failures") == "5" and len(cases) == 5 and
               all(case.find("failure") is not None for case in cases),
               "--junit writes one failing testcase per failure",
               junit.read_text())

    with tempfile.TemporaryDirectory() as directory:
        repository = Path(directory)
        tests = repository / "tests/ui/changed"
        shutil.copytree(FIXTURES / "changed", tests)
        git = ["git", "-C", str(repository), "-c", "user.name=t",
               "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
        subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["add", "."], check=True)
        subprocess.run(git + ["commit", "-qm", "base"], check=True)
        base = subprocess.run(git + ["rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True).stdout.strip()
        code, out = run(runner, "--changed", base, cwd=repository)
        expect(code == 0 and "no changed tests" in out,
               "--changed with no change runs nothing", out)
        (tests / "cheap.gti").write_text((tests / "cheap.gti").read_text() +
                                         "// edited\n")
        (tests / "other.stdout").write_text("x\n")
        code, out = run(runner, "--changed", base, "--list", cwd=repository)
        expect(code == 0 and "cheap.gti" in out and "other.gti" in out and
               "budget.gti" not in out,
               "--changed selects edited tests and their companions", out)
        (tests / "other.stdout").unlink()
        (tests / "budget.gti").write_text((tests / "budget.gti").read_text() +
                                          "// edited\n")
        code, out = run(runner, "--changed", base, cwd=repository,
                        env={"GTI_TEST_TIME_SCALE": "0.5"})
        budget = block(out, "FAIL", "budget.gti")
        expect(code == 1 and "over budget:" in budget and
               "fix: make it cheaper, or move it to tests/slow/" in budget and
               "bless:" not in budget and "RESULT fail: 1 passed, 1 failed"
               in out, "a changed test over half its limit fails, without a "
               "bless hint", out)
        code, out = run(runner, cwd=repository, env={"GTI_TEST_TIME_SCALE": "0.5"},
                        *("--lsp", "/nonexistent/gti_lsp"))
        expect(code == 0 and "tests/lsp skipped" not in out and
               "RESULT ok: 3 passed" in out,
               "without arguments every default root runs; old tests keep "
               "their full limit", out)
        slow = repository / "tests/slow/x"
        slow.mkdir(parents=True)
        (slow / "unexplained.gti").write_text("int main() { return 0; }\n")
        code, out = run(runner, "--tidy", cwd=repository)
        expect(code == 1 and "tests/slow/x/unexplained.gti: the first line" in out,
               "tidy covers tests/slow", out)
        (slow / "unexplained.gti").unlink()
        (repository / "tests/lsp").mkdir()
        code, out = run(runner, "--lsp", "/nonexistent/gti_lsp", "--list",
                        cwd=repository)
        expect("tests/lsp skipped" in out,
               "tests/lsp is skipped cleanly without gti_lsp", out)

    code, out = run(runner, FIXTURES / "stray")
    expect(code == 0 and "RESULT ok: 1 passed" in out,
           "a stray test.transcript neither runs nor hides the tests below it",
           out)
    code, out = run(runner, "--tidy", "--root", FIXTURES / "stray")
    expect(code == 1 and "a test.transcript belongs in a directory under a "
           "`project` directory" in out, "tidy reports a stray transcript", out)


def kinds_self_test(runner):
    """Parse-only modes, analysis options and snapshot kinds."""
    for isolate in ((), ("--isolate",)):
        code, out = run(runner, FIXTURES / "kinds", "--jobs", "4", *isolate)
        expect(code == 0 and "RESULT ok: 24 passed, 0 failed" in out,
               f"the other test kinds pass {isolate}", out)
    code, out = run(runner, "--tidy", "--root", FIXTURES / "kinds")
    expect(code == 0, "tidy accepts the kind fixtures", out)

    features_self_test(runner)
    defaults_self_test(runner)

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


def features_self_test(runner):
    """Native sections, signals and binary output: the failure paths."""
    code, out = run(runner, FIXTURES / "features/fail", "--jobs", "3")
    expect(code == 1 and "RESULT fail: 0 passed, 3 failed, 0 ice" in out,
           "failing feature fixtures fail (not as internal errors)", out)
    expect("expected: terminated by SIGSEGV\n  actual:   terminated by SIGABRT"
           in block(out, "FAIL", "wrong_signal.gti"),
           "the wrong signal is reported", out)
    expect("-a\\x00c\n      +a\\x00b" in block(out, "FAIL", "binary_mismatch.gti"),
           "binary stdout differences are shown escaped", out)
    native = block(out, "FAIL", "native_error.gti")
    expect("a C/C++ section did not compile" in native and
           "broken.c:1:27: error:" in native and "/case/" not in native,
           "a C section that does not compile shows the C compiler output", out)
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "bless"
        shutil.copytree(FIXTURES / "features/bless", work)
        # Replacing the inline `//@ stdout:` line needs --bless-directives.
        code, out = run(runner, work, "--bless", "--bless-directives")
        test = work / "nul_stdout.gti"
        expect(code == 0 and "stdout:" not in test.read_text() and
               (work / "nul_stdout.stdout").read_bytes() == b"x\0y\n",
               "bless writes NUL output to a sibling .stdout, not inline",
               out + test.read_text())
        code, out = run(runner, work)
        expect(code == 0, "the blessed binary output passes", out)


def defaults_self_test(runner):
    """gti-test.defaults files and sibling-directory imports (examples/)."""
    ok = FIXTURES / "defaults/ok"
    code, out = run(runner, ok / "warned.gti", ok / "scoped.gti",
                    ok / "imports.gti")
    expect(code == 0 and "RESULT ok: 3 passed, 0 failed" in out,
           "defaults allow warnings, scope a profile, and imports are copied",
           out)
    code, out = run(runner, FIXTURES / "defaults/bad")
    bad = block(out, "FAIL", "case.gti")
    expect(code == 1 and "gti-test.defaults:2: a defaults file sets only "
           "analysis options and allow-warnings, not 'run-pass'" in bad and
           "gti-test.defaults:3: a defaults file holds" in bad,
           "an invalid defaults file fails the tests below it", out)


def project_self_test(runner):
    """Project transcripts: commands in a copy of the directory."""
    code, out = run(runner, FIXTURES / "project/ok")
    expect(code == 0 and "RESULT ok: 1 passed, 0 failed" in out,
           "a project transcript passes", out)
    code, out = run(runner, FIXTURES / "project/fail", "--jobs", "3",
                    env={"GTI_TEST_TIME_SCALE": "0.1"})
    expect(code == 1 and "0 passed, 2 failed, 0 ice, 1 timeout" in out,
           "failing transcripts fail", out)
    wrong = block(out, "FAIL", "wrong_exit/test.transcript")
    expect("line 2: $ gti main.gti --emit-mir -o out.mir" in wrong and
           "exit code 65, expected 2" in wrong and
           "$DIR/main.gti:1:21: error[GTI-S2001]" in wrong and
           "bless:" in wrong,
           "a wrong exit code shows normalised output and offers bless", out)
    output = block(out, "FAIL", "wrong_output/test.transcript")
    expect('stdout lacks "no such text"' in output and
           'stdout contains "Diagnostics"' in output,
           "missing and forbidden output fail", out)
    expect("over the CPU limit" in block(out, "TIMEOUT", "spin/test.transcript"),
           "a spinning command exceeds the CPU budget", out)
    expect(not list((FIXTURES / "project").rglob("build")),
           "transcripts never write into the source tree")
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "project"
        shutil.copytree(FIXTURES / "project/bless", work)
        code, out = run(runner, work, "--bless")
        text = (work / "exit/test.transcript").read_text()
        expect(code == 0 and "BLESSED" in out and "\n? 65\n" in text and
               "stderr: $DIR/main.gti:1:21: error[GTI-S2001]" in text,
               "bless records exit codes and diagnostics", out + text)
        code, out = run(runner, work)
        expect(code == 0, "a blessed transcript passes", out)


def lsp_self_test(runner):
    """LSP marker tests and transcripts against a per-worker gti_lsp."""
    code, out = run(runner, FIXTURES / "lsp/ok", "--jobs", "2")
    expect(code == 0 and "RESULT ok: 10 passed, 0 failed" in out,
           "LSP fixtures pass: edit steps, server options, code actions, "
           "several files, a project, cursor markers", out)
    # One worker keeps a server per option list: the same call is checked
    # under the concurrent and the default profile in turn.
    code, out = run(runner, FIXTURES / "lsp/ok/server_concurrent.gti",
                    FIXTURES / "lsp/ok/server_single_threaded.gti",
                    FIXTURES / "lsp/ok/server_concurrent.gti", "--jobs", "1")
    expect(code == 0 and "RESULT ok: 2 passed" in out,
           "tests with different server options share a worker", out)
    code, out = run(runner, FIXTURES / "lsp/fail", "--jobs", "3")
    expect(code == 1 and "RESULT fail: 0 passed, 9 failed" in out,
           "failing LSP fixtures fail without timing out", out)
    actions = block(out, "FAIL", "wrong_code_actions.gti")
    for problem in ('line 2 (code-action): no code action titled "no such fix"; '
                    "offered: ",
                    "line 3 (code-action): the edited document differs from "
                    "step 2",
                    "-T identity<T>(T value) { int unused = 2; return value; }",
                    "line 4 (no-code-action): expected no code action",
                    "line 5 (code-action): no published diagnostic covers "
                    "@clean"):
        expect(problem in actions, f"code action failure reported: {problem}",
               out)
    navigation = block(out, "FAIL", "wrong_navigation.gti")
    for problem in ("line 2 (definition): expected @twin; got @lib.gti:twin",
                    "line 3 (definition): expected @std:std/thread.gti; got "
                    "@std:std/cthread.gti:",
                    "line 4 (definition): expected "
                    "@std:std/cthread.gti#no_such_symbol; got",
                    'line 5 (hover): hover contains "answer"',
                    'line 6 (signature): signature help does not contain '
                    '"double scale"',
                    "line 7 (completion-detail): detail of 'level' does not "
                    'contain "bool"',
                    "line 8 (hover): no marker @lib.gti:nosuch"):
        expect(problem in navigation, f"navigation failure reported: {problem}",
               out)
    expect("actual:   ERROR GTI-S2001 at step 2:2" in
           block(out, "FAIL", "wrong_step.gti"),
           "a step's diagnostic is located in that step", out)
    never = block(out, "FAIL", "never_arrives.jsonl")
    expect("line 8: gti_lsp fell idle" in never and "window/showMessage" in never
           and float(never.split("] ")[1].split("s")[0]) < 5,
           "an awaited message that never comes fails fast, naming it", out)
    never = block(out, "FAIL", "never_published.jsonl")
    expect("line 5: gti_lsp fell idle" in never and "window/logMessage" in never
           and float(never.split("] ")[1].split("s")[0]) < 5,
           "before any publication an idle server fails the wait fast", out)
    wrong = block(out, "FAIL", "wrong_answers.gti")
    for problem in ('line 2 (hover): hover does not contain "no such text"',
                    "line 3 (definition): expected @local; got @twice",
                    "line 4 (references): expected @twice; got @twice @call",
                    "line 5 (token): expected token variable, got function",
                    "line 6 (rename): expected edits at @local; got @local "
                    "@local_use",
                    "line 7 (completion): completion lacks 'absent'",
                    "line 7 (completion): completion offers 'level'",
                    "line 8 (symbols): document symbols lack 'missing_symbol'",
                    "line 9 (formatting): formatting changes the file"):
        expect(problem in wrong, f"LSP failure reported: {problem}", out)
    expect("actual:   ERROR GTI-S2001 at line 4" in
           block(out, "FAIL", "unannotated.gti"),
           "an unannotated published diagnostic fails", out)
    expect("no marker /*@nowhere*/" in block(out, "FAIL", "bad_marker.gti"),
           "an unknown marker is an invalid test", out)
    expect("line 3: expected a message matching" in
           block(out, "FAIL", "wrong_transcript.jsonl") and "seen: " in out,
           "a transcript mismatch shows what was seen", out)
    code, out = run(runner, FIXTURES / "lsp/ok/requests.gti", "--lsp",
                    "/usr/bin/true")
    expect(code == 1 and "could not start /usr/bin/true" in out,
           "a server that does not initialize fails the test", out)
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "lsp"
        shutil.copytree(FIXTURES / "lsp/fail", work)
        code, out = run(runner, work / "unannotated.gti", "--bless")
        expect(code == 0 and "BLESSED" in out and "//~ ERROR GTI-S2001" in
               (work / "unannotated.gti").read_text(),
               "bless annotates published diagnostics", out)


def main():
    runner = Path(sys.argv[1]).resolve()
    jobs = "4"
    if "--jobs" in sys.argv:
        jobs = sys.argv[sys.argv.index("--jobs") + 1]
    self_test(runner)
    print("gti-test self-test: ok", flush=True)
    if "--self-test-only" in sys.argv:
        return 0
    # Trees built without the language server (GTI_BUILD_LSP=OFF) skip it.
    roots = ["tests/ui", "tests/project"]
    if (runner.parent / "gti_lsp").exists():
        roots.append("tests/lsp")
    else:
        print("gti-test: no gti_lsp beside the runner; tests/lsp skipped")
    for root in roots:
        code, out = run(runner, "--tidy", "--root", ROOT / root,
                        fixtures=False)
        sys.stdout.write(out)
        if code != 0:
            return code
    code, out = run(runner, *(ROOT / root for root in roots), "--jobs",
                    jobs, fixtures=False)
    sys.stdout.write(out)
    return code


if __name__ == "__main__":
    sys.exit(main())
