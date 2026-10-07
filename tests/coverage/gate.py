#!/usr/bin/env python3
"""Gate the language coverage corpus, finding reproducers, and rejections.

Every program is compiled with the GTI compiler under test and classified:

  pass          compiled, ran, and exited with the expected status
  runtime       compiled, but exited with an unexpected status
  ice           GTI-B0001 internal compiler error
  backend       the generated C++ failed to compile
  link          the native link reported undefined symbols
  silent        nonzero compiler exit without any coded GTI diagnostic
  hang          the compiler or program exceeded its time limit
  crash         the compiler was killed by a signal
  bad-location  rejected, but a diagnostic has no source location
  bad-diagnostic  the output contains text the expectation forbids, or more
                occurrences of a text than its `at_most` limit
  nondeterministic  repeated emission (`deterministic`, e.g. "--emit-mir")
                produced different bytes
  rejected      rejected with located GTI diagnostics
  wrong-result  a project command's status or output differs (projects only)

`expectations.json` states the expected outcome of each program. Corpus
programs default to `pass`; reproducers, rejections and projects must be
listed. A project is a directory under `projects/`; it is copied to the output
directory and each of its `steps` runs there in order. A step gives a
`command` (for example `["test"]`), an optional `cwd` inside the copy, and
compares the process `exit` (an integer or "nonzero"), the strings in
`contains`, and an optional `pattern` regular expression. A single step may be
written directly in the entry. Any entry may list `forbid` strings that must not appear
in the compiler output (for example a misleading help text), `at_most` limits
on how often a text may appear (for example a duplicated error), and a compile
`timeout` in seconds (for example for a known hang), and extra compiler
`flags`. A program may instead list `cases`, each running the built program
with `args` and comparing its `exit`, exact `stdout`, and a `stderr` regular
expression; this checks defined runtime failure reports and cleanup order.

An entry without `finding` describes correct behavior. An entry with
`finding` is a known, unresolved defect: it records the outcome the defect
currently produces, and the gate fails when that outcome changes. A defect
that stops reproducing must have its record updated (FINDINGS.md, the
expectation, and any linked issue), so a fix is never silently unrecorded and
a failing test can stay in place without making the gate red.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GROUPS = {
    "programs": (ROOT, "c*.gti"),
    "reproducers": (ROOT / "reproducers", "*.gti"),
    "rejections": (ROOT / "rejections", "*.gti"),
    "projects": (ROOT / "projects", "*"),
}
OUTCOMES = {"pass", "runtime", "ice", "backend", "link", "silent", "hang",
            "crash", "bad-location", "bad-diagnostic", "rejected", "wrong-result",
            "nondeterministic"}
COMMON_FLAGS = ["--progress", "never", "--color", "never"]
COMPILE_SECONDS = 180
RUN_SECONDS = 60
CODE = re.compile(r"(?:error|warning)\[(GTI-[A-Z][0-9]{4})\]")
UNLOCATED = re.compile(r"^<source>:", re.MULTILINE)
BACKEND_LOG = re.compile(r"backend diagnostics saved at (\S+)")


def diagnostic_problem(text: str, expectation: dict) -> str | None:
    """Return why output violates `forbid` / `at_most`, or None."""
    for forbidden in expectation.get("forbid", []):
        if forbidden in text:
            return f"forbidden text {forbidden!r}"
    for needle, limit in expectation.get("at_most", {}).items():
        if text.count(needle) > limit:
            return f"{needle!r} appears {text.count(needle)} times (at most {limit})"
    return None


def classify_determinism(gti: Path, source: Path, output: Path, expectation: dict) -> tuple[str, str]:
    """Emit the same artifact repeatedly and require identical bytes."""
    flag = expectation["deterministic"]
    emitted = set()
    for run in range(expectation.get("runs", 6)):
        artifact = output.with_name(f"{output.name}.{run}")
        command = [str(gti), str(source), flag, "-o", str(artifact), *COMMON_FLAGS]
        try:
            compiled = subprocess.run(command, capture_output=True, text=True,
                                      timeout=expectation.get("timeout", COMPILE_SECONDS))
        except subprocess.TimeoutExpired:
            return "hang", "emission exceeded its time limit"
        if compiled.returncode != 0 or not artifact.is_file():
            return "rejected", " ".join(sorted(set(CODE.findall(compiled.stderr))))
        emitted.add(artifact.read_bytes())
    if len(emitted) == 1:
        return "pass", ""
    return "nondeterministic", f"{len(emitted)} distinct {flag} outputs"


def classify_project(gti: Path, project: Path, output: Path, expectation: dict) -> tuple[str, str]:
    """Run a project's steps in a scratch copy and compare status and output."""
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(project, output, ignore=shutil.ignore_patterns("build", "gti.lock"))
    steps = expectation.get("steps", [expectation])
    log = []
    for index, step in enumerate(steps):
        subcommand, *rest = step["command"]
        # Presentation flags must precede any "--" program-argument separator.
        command = [str(gti), subcommand, *COMMON_FLAGS, *rest]
        try:
            ran = subprocess.run(command, cwd=output / step.get("cwd", "."), capture_output=True,
                                 text=True, timeout=COMPILE_SECONDS + RUN_SECONDS)
        except subprocess.TimeoutExpired:
            return "hang", f"step {index} exceeded its time limit"
        text = ran.stdout + ran.stderr
        log.append(f"$ {' '.join(step['command'])}\n{text}")
        (output / "gate.log").write_text("\n".join(log), encoding="utf-8")
        if "GTI-B0001" in text:
            return "ice", f"step {index}: " + " ".join(sorted(set(CODE.findall(text))))
        problem = diagnostic_problem(text, step)
        if problem:
            return "bad-diagnostic", f"step {index}: {problem}"
        expected_exit = step.get("exit", 0)
        status_ok = (ran.returncode != 0) if expected_exit == "nonzero" else ran.returncode == expected_exit
        missing = [item for item in step.get("contains", []) if item not in text]
        if "pattern" in step and not re.search(step["pattern"], text, re.MULTILINE):
            missing.append(step["pattern"])
        if not status_ok or missing:
            return "wrong-result", f"step {index}: exit {ran.returncode}" + (f", missing {missing}" if missing else "")
    return "pass", ""


def classify(gti: Path, source: Path, output: Path, expectation: dict) -> tuple[str, str]:
    """Return (outcome, detail) for one program."""
    if "deterministic" in expectation:
        return classify_determinism(gti, source, output, expectation)
    expected_exit = expectation.get("exit", 0)
    compile_seconds = expectation.get("timeout", COMPILE_SECONDS)
    command = [str(gti)]
    if b"@coverage-profile: concurrent" in source.read_bytes():
        command += ["--execution-profile", "concurrent"]
    command += [str(source), "-o", str(output), *COMMON_FLAGS,
                *expectation.get("flags", [])]
    try:
        compiled = subprocess.run(command, capture_output=True, text=True,
                                  timeout=compile_seconds)
    except subprocess.TimeoutExpired:
        return "hang", f"compiler exceeded {compile_seconds}s"
    text = compiled.stdout + compiled.stderr
    output.with_suffix(".log").write_text(text, encoding="utf-8")
    codes = sorted(set(CODE.findall(text)))
    problem = diagnostic_problem(text, expectation)
    if problem:
        return "bad-diagnostic", " ".join(codes + [problem])
    if compiled.returncode == 0:
        cases = expectation.get("cases", [{"exit": expected_exit}])
        for index, case in enumerate(cases):
            try:
                # Run from the output directory so programs that write
                # relative files never touch the source tree.
                ran = subprocess.run([str(output), *case.get("args", [])], capture_output=True,
                                     text=True, timeout=RUN_SECONDS, cwd=output.parent)
            except subprocess.TimeoutExpired:
                return "hang", f"case {index} exceeded {RUN_SECONDS}s"
            if ran.returncode != case.get("exit", 0):
                return "runtime", f"case {index}: exit {ran.returncode}, expected {case.get('exit', 0)}"
            if "stdout" in case and ran.stdout != case["stdout"]:
                return "runtime", f"case {index}: stdout {ran.stdout!r}"
            if "stderr" in case and not re.fullmatch(case["stderr"], ran.stderr, re.DOTALL):
                return "runtime", f"case {index}: stderr {ran.stderr!r}"
        return "pass", ""
    if compiled.returncode < 0:
        return "crash", f"compiler signal {-compiled.returncode}"
    if "GTI-B0001" in codes:
        return "ice", " ".join(codes)
    # The driver saves native compiler/linker output to a file and names it.
    saved = BACKEND_LOG.search(text)
    if saved and Path(saved.group(1)).is_file():
        text += Path(saved.group(1)).read_text(encoding="utf-8", errors="replace")
    if "Undefined symbols" in text or "undefined reference" in text:
        return "link", ""
    if "native C++ compiler" in text or "backend failed" in text:
        return "backend", ""
    if not codes:
        return "silent", f"exit {compiled.returncode}"
    if UNLOCATED.search(text):
        return "bad-location", " ".join(codes)
    return "rejected", " ".join(codes)


def check(name: str, expectation: dict, outcome: str, detail: str) -> str | None:
    """Return a failure message, or None when the outcome meets expectation."""
    expected = expectation.get("outcome", "pass")
    finding = expectation.get("finding")
    code = expectation.get("code")
    code_matches = not code or code in detail.split()
    if outcome != expected or (finding and not code_matches):
        if finding:
            return (f"{name}: known finding {finding} now produces '{outcome}' "
                    f"({detail or 'no detail'}), recorded as '{expected}'. If it is "
                    f"resolved, update FINDINGS.md, expectations.json and any "
                    f"linked issue.")
        return f"{name}: expected '{expected}', got '{outcome}' ({detail or 'no detail'})"
    if not code_matches:
        return f"{name}: expected diagnostic {code}, got {detail or 'none'}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("gti", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("GTI_COVERAGE_JOBS", "2")))
    args = parser.parse_args()
    expectations = json.loads((ROOT / "expectations.json").read_text(encoding="utf-8"))

    work = []
    failures = []
    for group, (directory, pattern) in GROUPS.items():
        listed = dict(expectations.get(group, {}))
        for source in sorted(directory.glob(pattern)):
            if group == "projects":
                if not source.is_dir():
                    continue
                name = source.name
            else:
                name = source.stem
            if group != "programs" and name not in listed:
                failures.append(f"{group}/{name}: missing from expectations.json")
                continue
            expectation = listed.pop(name, {})
            if expectation.get("outcome", "pass") not in OUTCOMES:
                failures.append(f"{group}/{name}: unknown outcome {expectation['outcome']!r}")
                continue
            work.append((group, name, source, expectation))
        failures += [f"{group}/{name}: listed but no such file" for name in listed]

    args.output.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}

    def run(item):
        group, name, source, expectation = item
        target = args.output / f"{group}-{name}"
        if group == "projects":
            outcome, detail = classify_project(args.gti, source, target, expectation)
        else:
            outcome, detail = classify(args.gti, source, target, expectation)
        return group, name, expectation, outcome, detail

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for group, name, expectation, outcome, detail in pool.map(run, work):
            key = "known" if expectation.get("finding") else outcome
            counts[key] = counts.get(key, 0) + 1
            message = check(f"{group}/{name}", expectation, outcome, detail)
            if message:
                failures.append(message)

    summary = ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
    print(f"coverage gate: {len(work)} programs ({summary}); logs in {args.output}")
    for failure in failures:
        print("FAIL " + failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
