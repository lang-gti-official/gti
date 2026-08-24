#!/usr/bin/env python3

"""Run the repository's own programs at every optimization level and compare.

`optimizer_differential.py` has an independent oracle but a narrow corpus: the
model it generates from is int32/bool arithmetic with no classes, arrays,
generics, lambdas or `expected`. This trades the oracle for the corpus. It
takes programs already in the repository — `examples/` and, with `--fixtures`,
`tests/fixtures/` — and requires each to behave identically at `-O0`, `-O1`,
`-O2` and `-O3`.

    python3 scripts/debug/corpus_differential.py
    python3 scripts/debug/corpus_differential.py --fixtures --jobs 4
    python3 scripts/debug/corpus_differential.py --levels O0 O3 --keep

No expected output is needed, which is the point: optimization must not change
observable behaviour, so a disagreement between levels is a bug regardless of
which level is right. The cost is that a fault every level shares is invisible
here — that is what the model-based oracle is for, and the two are
complementary rather than redundant.

Nondeterministic programs would otherwise look like divergence, so each
program is first run twice at the baseline level; one that disagrees with
itself is reported as `nondet` and excluded rather than blamed on the
optimizer.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent
LEVELS = ["O0", "O1", "O2", "O3"]


@dataclass(frozen=True)
class Observation:
    """What running a program produced, or why it could not be run."""

    state: str          # "ran" | "build-failed" | "hang" | "crash"
    status: int | None
    stdout: str
    detail: str

    @property
    def behaviour(self) -> tuple[int | None, str]:
        return (self.status, self.stdout)


def build_and_run(gti: Path, cxx: str, source: Path, output: Path,
                  level: str, timeout: int) -> Observation:
    command = [str(gti), str(source), f"-{level}", "--std", "c++23",
               "-o", str(output), "--cxx", cxx]
    try:
        built = subprocess.run(command, capture_output=True, text=True,
                               timeout=timeout)
    except subprocess.TimeoutExpired:
        return Observation("build-failed", None, "", f"{level}: build timed out")
    if built.returncode != 0:
        output_text = built.stderr + built.stdout
        first = (built.stderr or built.stdout).strip().split("\n")
        # Prefix the code explicitly: the first line is truncated below, and
        # a long path can push the diagnostic code past the cut.
        code = "GTI-B0001 " if "GTI-B0001" in output_text else ""
        return Observation("build-failed", None, "",
                           f"{level}: {code}{first[0][:160] if first else ''}")
    try:
        ran = subprocess.run([str(output)], capture_output=True, text=True,
                             timeout=timeout)
    except subprocess.TimeoutExpired:
        return Observation("hang", None, "", f"{level}: did not exit")
    if ran.returncode < 0:
        return Observation("crash", None, "", f"{level}: signal {-ran.returncode}")
    return Observation("ran", ran.returncode, ran.stdout, "")


def compare_program(gti: Path, cxx: str, scratch: Path, source: Path,
                    levels: list[str], timeout: int) -> tuple[str, str]:
    """Return (verdict, detail) for one program across all levels."""
    stem = source.stem
    baseline = levels[0]

    first = build_and_run(gti, cxx, source, scratch / f"{stem}-{baseline}-a",
                          baseline, timeout)
    if first.state != "ran":
        # Not every program is buildable in a bare checkout — some want a
        # package or a target, and some fixtures are negative tests. None of
        # that is an optimizer finding. An *internal* error is different: a
        # silent "skipped" there hides a GTI-B0001 behind a count, which is
        # how #112 nearly went unnoticed.
        if first.state == "build-failed":
            if "GTI-B0001" in first.detail:
                return "ICE", first.detail
            return "skipped", first.detail
        return first.state, first.detail

    # A program that disagrees with itself cannot testify about the optimizer.
    repeat = build_and_run(gti, cxx, source, scratch / f"{stem}-{baseline}-b",
                           baseline, timeout)
    if repeat.state != "ran" or repeat.behaviour != first.behaviour:
        return "nondet", f"differs between two runs at {baseline}"

    for level in levels[1:]:
        other = build_and_run(gti, cxx, source, scratch / f"{stem}-{level}",
                              level, timeout)
        if other.state != "ran":
            return other.state, other.detail
        if other.behaviour != first.behaviour:
            return "DIVERGENT", (
                f"{baseline} exits {first.status} / {level} exits "
                f"{other.status}"
                + ("" if other.stdout == first.stdout else "; stdout differs"))
    return "ok", ""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare repository programs across optimization levels.")
    parser.add_argument("--gti", default="build/gti")
    parser.add_argument("--cxx", default=None)
    parser.add_argument("--levels", nargs="+", default=LEVELS)
    parser.add_argument("--fixtures", action="store_true",
                        help="also sweep tests/fixtures/")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--keep", action="store_true")
    arguments = parser.parse_args()

    gti = (REPOSITORY_ROOT / arguments.gti).resolve()
    if not gti.is_file():
        print(f"error: {gti} not found; build it first", file=sys.stderr)
        return 1
    cxx = arguments.cxx or shutil.which("c++") or "c++"

    sources = sorted((REPOSITORY_ROOT / "examples").glob("*.gti"))
    if arguments.fixtures:
        sources += sorted((REPOSITORY_ROOT / "tests" / "fixtures").glob("*.gti"))
    if not sources:
        print("error: no programs found", file=sys.stderr)
        return 1

    scratch = Path(tempfile.mkdtemp(prefix="gti-corpus-diff-"))
    tally: dict[str, int] = {}
    divergent: list[tuple[str, str]] = []

    def run_one(source: Path) -> tuple[Path, str, str]:
        verdict, detail = compare_program(gti, cxx, scratch, source,
                                          arguments.levels, arguments.timeout)
        return source, verdict, detail

    with ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        for source, verdict, detail in pool.map(run_one, sources):
            tally[verdict] = tally.get(verdict, 0) + 1
            if verdict == "DIVERGENT":
                divergent.append((source.name, detail))
                print(f"  {source.name:<40} DIVERGENT  {detail}")
            elif verdict in {"hang", "crash", "nondet", "ICE"}:
                print(f"  {source.name:<40} {verdict:<10} {detail}")

    print()
    summary = ", ".join(f"{count} {state}"
                        for state, count in sorted(tally.items()))
    print(f"{len(sources)} program(s) at [{' '.join(arguments.levels)}]: {summary}")
    if divergent:
        print(f"\n{len(divergent)} program(s) behave differently across "
              f"levels; sources kept in {scratch}")
        return 1
    if arguments.keep:
        print(f"artifacts kept in {scratch}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
