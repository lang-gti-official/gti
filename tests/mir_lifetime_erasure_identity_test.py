#!/usr/bin/env python3
"""RFC 0004 gate: lifetimes never change MIR or machine code for lifetime-free
programs.

Compiles every example with a reference compiler (built from the main commit
the lifetime change is based on) and a candidate compiler. For every example
the reference compiles, the candidate must produce:

  mir          byte-identical `--emit-mir --mir-normalize-loans` output. The
               normalizer erases only loan bookkeeping: semantic loan numbers,
               EndBorrow placement and the instruction-ID shifts it causes
               (src/compiler/mir_loan_normalization.h). Drops, cleanup and
               every other MIR fact still compare exactly.
  llvm-ir-O0   byte-identical `--emit-llvm -O0` output, except that each
               compiler's own standard-library root is spelled `<stdlib>`:
               `!gti.source` provenance names absolute source paths, and the
               two compilers' libraries live in different checkouts. Source
               offsets and everything else still compare exactly.
  llvm-ir-O3   likewise at -O3.
  run-O0       the same exit code and stdout from the native LLVM executable.
  run-O3       likewise at -O3.

A step the reference cannot perform (for example an LLVM limitation) must fail
in the candidate with the same exit code. Each category is reported
separately. A program whose reference executable gives different results on
two runs is reported as nondeterministic, not as a difference; its LLVM IR is
still compared. An example that needs the concurrent execution profile is
compiled with it by both compilers. Examples whose lifetime syntax the
reference cannot parse are compiled by the candidate only.

Usage: mir_lifetime_erasure_identity_test.py REFERENCE_GTI REFERENCE_STDLIB
                                             CANDIDATE_GTI CANDIDATE_STDLIB
                                             EXAMPLES_DIR [--jobs N]
                                             [--filter REGEX] [--keep DIR]
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Lifetime syntax: a `lifetime` binder, a reference or type region argument
# (`&<a>`, `<static>`), `for<lifetime ...>`, or an `outlives<...>` requirement.
LIFETIME_SYNTAX = re.compile(
    r"\blifetime\s+[A-Za-z_]\w*|&<\s*[A-Za-z_]\w*\s*>|\boutlives\s*<")

PROFILES = ([], ["--execution-profile", "concurrent"])
LEVELS = ("O0", "O3")
CATEGORIES = ("mir", *(f"llvm-ir-{level}" for level in LEVELS),
              *(f"run-{level}" for level in LEVELS))
COMPILE_TIMEOUT = 600
RUN_TIMEOUT = 60


def uses_lifetime_syntax(path: Path) -> bool:
    return LIFETIME_SYNTAX.search(path.read_text(encoding="utf-8")) is not None


class Compiler:
    def __init__(self, role: str, executable: str, stdlib: str):
        self.role = role
        self.executable = executable
        self.environment = dict(os.environ, GTI_STDLIB_PATH=stdlib)
        roots = {stdlib.rstrip("/"), os.path.realpath(stdlib)}
        self.stdlib_roots = sorted((root.encode() + b"/" for root in roots),
                                   key=len, reverse=True)

    def location_neutral(self, text: bytes) -> bytes:
        """Spell this compiler's standard-library root `<stdlib>`."""
        for root in self.stdlib_roots:
            text = text.replace(root, b"<stdlib>/")
        return text

    def compile(self, source: Path, arguments: list[str],
                output: Path) -> tuple[int, bytes]:
        """Exit code and artifact bytes (empty unless the compile succeeded)."""
        try:
            completed = subprocess.run(
                [self.executable, str(source), *arguments, "-o", str(output)],
                env=self.environment, capture_output=True,
                timeout=COMPILE_TIMEOUT)
        except subprocess.TimeoutExpired:
            return -1, b""
        if completed.returncode != 0 or not output.exists():
            return completed.returncode or -2, b""
        return 0, output.read_bytes()


def run(executable: Path, root: Path) -> tuple[int, bytes]:
    try:
        completed = subprocess.run(
            [str(executable)], cwd=root, stdin=subprocess.DEVNULL,
            capture_output=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return -1, b"<timeout>"
    return completed.returncode, completed.stdout


def first_difference(left: bytes, right: bytes) -> str:
    left_lines = left.splitlines()
    right_lines = right.splitlines()
    for index, (a, b) in enumerate(zip(left_lines, right_lines)):
        if a != b:
            return f"first difference at line {index + 1}"
    return (f"line counts {len(left_lines)} (reference) and "
            f"{len(right_lines)} (candidate)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("reference_stdlib")
    parser.add_argument("candidate")
    parser.add_argument("candidate_stdlib")
    parser.add_argument("examples")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--filter", default="",
                        help="only examples whose stem matches this regex")
    parser.add_argument("--keep", type=Path,
                        help="keep every artifact under this directory")
    arguments = parser.parse_args()

    examples_dir = Path(arguments.examples).resolve()
    selected = re.compile(arguments.filter)
    examples = [example for example in sorted(examples_dir.glob("*.gti"))
                if selected.search(example.stem)]
    if not examples:
        print("no examples found", file=sys.stderr)
        return 1
    reference = Compiler("reference", arguments.reference,
                         arguments.reference_stdlib)
    candidate = Compiler("candidate", arguments.candidate,
                         arguments.candidate_stdlib)
    # Examples run from the repository root, as the runtime harnesses do.
    run_root = examples_dir.parent

    failures: dict[str, list[str]] = collections.defaultdict(list)
    nondeterministic: list[str] = []
    timings: list[tuple[float, str]] = []
    compared = 0
    concurrent_examples = 0
    lifetime_examples: list[str] = []
    skipped: list[str] = []

    scratch_context = (tempfile.TemporaryDirectory() if arguments.keep is None
                       else None)
    scratch = (Path(scratch_context.name) if scratch_context
               else arguments.keep.resolve())
    scratch.mkdir(parents=True, exist_ok=True)

    def check(example: Path):
        started = time.monotonic()
        name = example.stem
        directory = scratch / name
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        result = {"name": name, "failures": [], "nondeterministic": [],
                  "kind": "compared", "concurrent": False}

        def artifact(compiler: Compiler, label: str) -> Path:
            return directory / f"{compiler.role}.{label}"

        normalized = ["--emit-mir", "--mir-normalize-loans"]
        profile = None
        reference_mir = (1, b"")
        for candidate_profile in PROFILES:
            reference_mir = reference.compile(
                example, [*normalized, *candidate_profile],
                artifact(reference, "mir"))
            if reference_mir[0] == 0:
                profile = candidate_profile
                break
        if profile is None:
            # The reference cannot compile it: either lifetime syntax the
            # reference predates, or not a compilable example at all.
            if uses_lifetime_syntax(example):
                result["kind"] = "lifetime"
                if not any(candidate.compile(example, [*normalized, *p],
                                             artifact(candidate, "mir"))[0] == 0
                           for p in PROFILES):
                    result["failures"].append(
                        ("mir", "candidate failed to compile a lifetime example"))
            else:
                result["kind"] = "skipped"
            result["seconds"] = time.monotonic() - started
            return result
        result["concurrent"] = bool(profile)

        def compare(arguments_: list[str], label: str):
            expected = reference.compile(example, [*arguments_, *profile],
                                         artifact(reference, label))
            actual = candidate.compile(example, [*arguments_, *profile],
                                       artifact(candidate, label))
            return expected, actual

        candidate_mir = candidate.compile(example, [*normalized, *profile],
                                          artifact(candidate, "mir"))
        if candidate_mir[0] != 0:
            result["failures"].append(
                ("mir", f"candidate failed (exit {candidate_mir[0]}) where "
                        "the reference compiles"))
        elif candidate_mir[1] != reference_mir[1]:
            result["failures"].append(
                ("mir", "normalized MIR differs: " +
                 first_difference(reference_mir[1], candidate_mir[1])))

        for level in LEVELS:
            category = f"llvm-ir-{level}"
            expected, actual = compare(["--emit-llvm", f"-{level}"],
                                       f"{level}.ll")
            if expected[0] != actual[0]:
                result["failures"].append(
                    (category, f"exit {actual[0]}, reference exit {expected[0]}"))
            elif (reference.location_neutral(expected[1]) !=
                  candidate.location_neutral(actual[1])):
                result["failures"].append(
                    (category, "LLVM IR differs: " +
                     first_difference(expected[1], actual[1])))

        for level in LEVELS:
            category = f"run-{level}"
            build = ["--backend", "llvm", f"-{level}"]
            expected_build, actual_build = compare(build, f"{level}.exe")
            if expected_build[0] != actual_build[0]:
                result["failures"].append(
                    (category, f"native build exit {actual_build[0]}, "
                               f"reference exit {expected_build[0]}"))
                continue
            if expected_build[0] != 0:
                continue
            expected_run = run(artifact(reference, f"{level}.exe"), run_root)
            actual_run = run(artifact(candidate, f"{level}.exe"), run_root)
            if expected_run == actual_run:
                continue
            if run(artifact(reference, f"{level}.exe"), run_root) != expected_run:
                result["nondeterministic"].append(category)
                continue
            detail = []
            if expected_run[0] != actual_run[0]:
                detail.append(f"exit {actual_run[0]}, reference exit "
                              f"{expected_run[0]}")
            if expected_run[1] != actual_run[1]:
                detail.append("stdout differs: " +
                              first_difference(expected_run[1], actual_run[1]))
            result["failures"].append((category, "; ".join(detail)))
        result["seconds"] = time.monotonic() - started
        return result

    started = time.monotonic()
    try:
        with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
            for result in pool.map(check, examples):
                name = result["name"]
                timings.append((result["seconds"], name))
                if result["kind"] == "lifetime":
                    lifetime_examples.append(name)
                elif result["kind"] == "skipped":
                    skipped.append(name)
                else:
                    compared += 1
                    concurrent_examples += result["concurrent"]
                for category, detail in result["failures"]:
                    failures[category].append(f"{name}: {detail}")
                for category in result["nondeterministic"]:
                    nondeterministic.append(f"{name}: {category}")
    finally:
        if scratch_context:
            scratch_context.cleanup()
    elapsed = time.monotonic() - started

    for category in CATEGORIES:
        entries = failures.get(category, [])
        print(f"[{category}] {len(entries)} difference(s)")
        for entry in entries:
            print(f"  {entry}")
    if nondeterministic:
        print(f"[nondeterministic] {len(nondeterministic)} run(s) whose "
              "reference output varies between runs (not compared)")
        for entry in nondeterministic:
            print(f"  {entry}")
    if skipped:
        print(f"[skipped] {len(skipped)} example(s) the reference does not "
              f"compile: {', '.join(skipped)}")
    total = sum(len(entries) for entries in failures.values())
    timings.sort(reverse=True)
    slowest = ", ".join(f"{name} {seconds:.1f}s"
                        for seconds, name in timings[:3])
    print(f"compared {compared} lifetime-free examples "
          f"({concurrent_examples} under the concurrent profile); "
          f"{len(lifetime_examples)} lifetime examples compiled by the "
          f"candidate only; {total} difference(s)")
    print(f"wall {elapsed:.1f}s with {arguments.jobs} jobs; per example "
          f"{sum(seconds for seconds, _ in timings) / len(timings):.1f}s "
          f"average (slowest: {slowest})")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
