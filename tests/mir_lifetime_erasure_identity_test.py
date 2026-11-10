#!/usr/bin/env python3
"""RFC 0004 gate: lifetimes never change MIR for lifetime-free programs.

Compiles every example with a reference compiler (built from the main commit
this branch is based on) and a candidate compiler, emitting verified MIR, and
requires byte-identical output for every example the reference compiles.
Examples whose lifetime syntax the reference cannot parse are compiled by the
candidate only.

Usage: mir_lifetime_erasure_identity_test.py REFERENCE_GTI REFERENCE_STDLIB
                                             CANDIDATE_GTI CANDIDATE_STDLIB
                                             EXAMPLES_DIR [--jobs N]
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Lifetime syntax: a `lifetime` binder, a reference or type region argument
# (`&<a>`, `<static>`), `for<lifetime ...>`, or an `outlives<...>` requirement.
LIFETIME_SYNTAX = re.compile(
    r"\blifetime\s+[A-Za-z_]\w*|&<\s*[A-Za-z_]\w*\s*>|\boutlives\s*<")


def uses_lifetime_syntax(path: Path) -> bool:
    return LIFETIME_SYNTAX.search(path.read_text(encoding="utf-8")) is not None


def emit_mir(compiler: str, stdlib: str, source: Path, output: Path) -> tuple[int, bytes]:
    environment = dict(os.environ, GTI_STDLIB_PATH=stdlib)
    completed = subprocess.run(
        [compiler, str(source), "--emit-mir", "-o", str(output)],
        env=environment, capture_output=True, timeout=600)
    data = output.read_bytes() if completed.returncode == 0 and output.exists() else b""
    return completed.returncode, data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("reference_stdlib")
    parser.add_argument("candidate")
    parser.add_argument("candidate_stdlib")
    parser.add_argument("examples")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    arguments = parser.parse_args()

    examples = sorted(Path(arguments.examples).glob("*.gti"))
    if not examples:
        print("no examples found", file=sys.stderr)
        return 1

    failures: list[str] = []
    compared = 0
    lifetime_examples: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        scratch_path = Path(scratch)

        def check(example: Path) -> tuple[str, str | None, bool]:
            name = example.stem
            candidate_out = scratch_path / f"{name}.candidate.mir"
            candidate_code, candidate = emit_mir(
                arguments.candidate, arguments.candidate_stdlib, example, candidate_out)
            reference_out = scratch_path / f"{name}.reference.mir"
            reference_code, reference = emit_mir(
                arguments.reference, arguments.reference_stdlib, example, reference_out)
            if reference_code != 0:
                # The reference cannot compile it: either lifetime syntax the
                # reference predates, or not a runnable example at all.
                if uses_lifetime_syntax(example):
                    if candidate_code != 0:
                        return name, "candidate failed to compile a lifetime example", True
                    return name, None, True
                return name, None, False
            if candidate_code != 0:
                return name, "candidate failed where the reference compiles", False
            if candidate != reference:
                return name, "MIR differs from the reference", False
            return name, None, False

        with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
            for name, failure, lifetime in pool.map(check, examples):
                if lifetime:
                    lifetime_examples.append(name)
                else:
                    compared += 1
                if failure:
                    failures.append(f"{name}: {failure}")

    for failure in failures:
        print(failure)
    print(f"compared {compared} lifetime-free examples; "
          f"{len(lifetime_examples)} lifetime examples compiled by the candidate only")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
