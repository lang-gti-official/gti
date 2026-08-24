#!/usr/bin/env python3

"""Differential-test GTI's own optimizer across every level it accepts.

Optimization must not change what a program computes. This builds each
generated program at `-O0`, `-O1`, `-O2` and `-O3`, runs all four, and
requires every exit status to agree — with each other *and* with the value
the model says the program returns.

    python3 scripts/debug/optimizer_differential.py                 # 24 cases
    python3 scripts/debug/optimizer_differential.py --count 100
    python3 scripts/debug/optimizer_differential.py --levels O0 O2
    python3 scripts/debug/optimizer_differential.py --seed 7 --keep

Two things make this worth having beyond the existing coverage.

`scripts/local_language_audit.py` builds at `O0` and `O3` only, and the
pipeline fuzz harness exercises `O2` for crashes rather than for behaviour, so
**`-O1` and `-O2` currently have no behavioural differential at all** — a
level-specific miscompilation there would be silent.

And an independent oracle matters more than a pairwise one. Comparing levels
to each other catches divergence but not a fault every level shares; comparing
against `Program.evaluate()`, which is computed in Python from the model and
never touches the compiler, catches that too. A row can therefore fail as
`divergent` (levels disagree) or `wrong` (levels agree on the wrong answer),
and the two point at very different bugs.

Each case also carries a metamorphic twin — a transformed program the model
proves returns the same value — so both are built and checked at every level.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent
LEVELS = ["O0", "O1", "O2", "O3"]


def load_model():
    """Import the shared program model by path, as the audit script does."""
    location = Path(__file__).resolve().parent / "typed_program_model.py"
    spec = importlib.util.spec_from_file_location("typed_program_model", location)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {location}")
    module = importlib.util.module_from_spec(spec)
    # Register before executing: @dataclass resolves its own module through
    # sys.modules, and fails outright when the entry is missing.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def build_and_run(gti: Path, cxx: str, source: Path, output: Path,
                  level: str) -> tuple[str, int | None, str]:
    """Build one program at one level and run it.

    Returns (state, exit status, detail). `state` is "ran" when the program
    built and ran to completion; anything else is a harness or compiler
    failure that the caller reports rather than compares.
    """
    command = [str(gti), str(source), f"-{level}", "--std", "c++23",
               "-o", str(output), "--cxx", cxx]
    built = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if built.returncode != 0:
        detail = (built.stderr or built.stdout).strip().split("\n")
        return "build-failed", None, detail[0][:200] if detail else ""
    try:
        ran = subprocess.run([str(output)], capture_output=True, text=True,
                             timeout=120)
    except subprocess.TimeoutExpired:
        return "hang", None, "the program did not exit within 120s"
    if ran.returncode < 0:
        return "crash", None, f"signal {-ran.returncode}"
    return "ran", ran.returncode, ""


def check_program(gti: Path, cxx: str, directory: Path, name: str,
                  source_text: str, expected: int,
                  levels: list[str]) -> tuple[str, str]:
    """Build one program at every level; return (verdict, detail)."""
    source = directory / f"{name}.gti"
    source.write_text(source_text, encoding="utf-8")

    results: dict[str, int] = {}
    for level in levels:
        state, status, detail = build_and_run(
            gti, cxx, source, directory / f"{name}-{level}", level)
        if state != "ran":
            return state, f"{level}: {detail}"
        results[level] = status

    distinct = set(results.values())
    if len(distinct) > 1:
        spread = ", ".join(f"{lvl}={results[lvl]}" for lvl in levels)
        return "divergent", f"levels disagree ({spread}); model says {expected}"
    only = distinct.pop()
    if only != expected:
        return "wrong", f"every level returned {only}; model says {expected}"
    return "ok", ""


INVARIANTS = Path(__file__).resolve().parent / "optimizer_invariants"
STATUS_HEADER = re.compile(r"//\s*expected exit status:\s*(\d+)")


def run_invariants(gti: Path, cxx: str, directory: Path,
                   levels: list[str]) -> list[tuple[str, str, str]]:
    """Check the hand-written optimizer invariants at every level.

    The generated corpus is int32/bool arithmetic, so it cannot express the
    properties an optimizer is most likely to break: that a defined runtime
    failure survives, and that a write through an aliasing reference is
    observed. These are written by hand and declare their expected exit
    status in a first-line comment.
    """
    failures: list[tuple[str, str, str]] = []
    for source in sorted(INVARIANTS.glob("*.gti")):
        with source.open(encoding="utf-8") as handle:
            match = STATUS_HEADER.match(handle.readline().strip())
        if match is None:
            failures.append((source.name, "no-header",
                             "first line must declare the expected status"))
            continue
        expected = int(match.group(1))
        verdict, detail = check_program(
            gti, cxx, directory, source.stem,
            source.read_text(encoding="utf-8"), expected, levels)
        if verdict != "ok":
            failures.append((source.name, verdict, detail))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that GTI's optimizer preserves program behaviour.")
    parser.add_argument("--gti", default="build/gti")
    parser.add_argument("--cxx", default=None,
                        help="C++ compiler for the backend (default: c++)")
    parser.add_argument("--count", type=int, default=24)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--levels", nargs="+", default=LEVELS)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--skip-invariants", action="store_true",
                        help="only run the generated corpus")
    arguments = parser.parse_args()

    gti = (REPOSITORY_ROOT / arguments.gti).resolve()
    if not gti.is_file():
        print(f"error: {gti} not found; build it first", file=sys.stderr)
        return 1
    cxx = arguments.cxx or shutil.which("c++") or "c++"

    model = load_model()
    directory = Path(tempfile.mkdtemp(prefix="gti-optimizer-diff-"))
    failures: list[tuple[str, str, str]] = []
    checked = 0

    if not arguments.skip_invariants:
        for name, verdict, detail in run_invariants(
                gti, cxx, directory, arguments.levels):
            failures.append((name, verdict, detail))
            print(f"  {name:<34} {verdict.upper():<13} {detail}")
        checked += len(list(INVARIANTS.glob("*.gti")))

    for index in range(arguments.count):
        case = model.generated_case(index, arguments.seed)
        try:
            expected = case.expected
        except model.ModelError as error:
            # The model contradicting itself is a generator bug, not a
            # compiler one; say so rather than blaming the compiler.
            failures.append((f"case{index}", "model-error", str(error)))
            continue

        # The metamorphic twin must return the same value, so both are checked.
        for label, program in (("original", case.original),
                               ("transformed", case.transformed)):
            name = f"case{index}-{label}"
            verdict, detail = check_program(
                gti, cxx, directory, name, program.emit("gti"), expected,
                arguments.levels)
            checked += 1
            if verdict != "ok":
                failures.append((name, verdict, detail))
                print(f"  {name:<26} {verdict.upper():<13} {detail}")

    print()
    levels = " ".join(arguments.levels)
    if failures:
        print(f"{len(failures)} failure(s) across {checked} program(s) "
              f"at [{levels}]; sources kept in {directory}")
        return 1
    if arguments.keep:
        print(f"sources kept in {directory}")
    print(f"{checked} program(s) agree at every level [{levels}] "
          "and match the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
