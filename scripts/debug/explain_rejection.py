#!/usr/bin/env python3

"""Explain *why* a program raises GTI-B0001, by naming the exact gate.

`GTI-B0001` says a body is "outside the complete C++ text vocabulary" or that
MIR verification failed, but not which of the hundreds of admission checks
rejected it. Finding that by hand means instrumenting the compiler, rebuilding,
running, reading, and reverting — the loop this script automates.

    python3 scripts/debug/explain_rejection.py --source bad.gti
    python3 scripts/debug/explain_rejection.py --source bad.gti --gate verifier

It temporarily rewrites one compiler source file so every rejection point
reports its line, rebuilds, runs the reproducer, prints the gates that fired
with their guarding comment and code, and then **always** restores the file.
The restore runs from a `finally` block and is verified before exit, so an
interrupted run cannot leave the tree instrumented.

Two gates cover every GTI-B0001 observed so far:

  emitter  (default)  CppMirBodyEmitter::supportsBodyTextImpl in
                      src/compiler/cpp_mir_body_emitter.cpp — decides whether a
                      verified MIR body can be written as C++.
  verifier            MIR verification error sites in src/compiler/mir.cpp —
                      decides whether the MIR itself is well formed.

Reading the output: many emitter sites sit inside speculative `is it shape X?`
lambdas where returning false only means "try the next shape". Those are
reported as `(probe)` and are usually not the cause; a gate in the function
body proper is. When several fire, the decisive one is normally the last, or
the one whose guarding comment describes the construct in your reproducer.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent

GATES = {
    "emitter": {
        "file": Path("src/compiler/cpp_mir_body_emitter.cpp"),
        "function": "bool CppMirBodyEmitter::supportsBodyTextImpl(",
        "pattern": "return false;",
        "environment": "GTI_EXPLAIN_REJECT",
        "marker": "GTI_REJECT",
    },
    "verifier": {
        "file": Path("src/compiler/mir.cpp"),
        "function": None,  # error sites are scattered; instrument all of them
        "pattern": "result.errors.push_back(",
        "environment": "GTI_EXPLAIN_REJECT",
        "marker": "GTI_REJECT",
    },
}


class ExplainError(Exception):
    """A usage or environment problem."""


def brace_range(lines: list[str], start_index: int) -> int:
    """Index of the line closing the function opened at `start_index`."""
    depth = 0
    seen_open = False
    for index in range(start_index, len(lines)):
        depth += lines[index].count("{") - lines[index].count("}")
        if lines[index].count("{"):
            seen_open = True
        if seen_open and depth <= 0:
            return index
    return len(lines) - 1


def instrument(path: Path, gate: dict) -> tuple[int, dict[int, int], list[str]]:
    """Rewrite rejection points so each reports its line.

    Returns the number of instrumented points, a map from instrumented line
    number to original line number, and the instrumented text as lines.
    Reported locations are translated through that map so they name the
    pristine file the reader will actually open.
    """
    original = path.read_text().split("\n")

    if gate["function"] is not None:
        starts = [i for i, line in enumerate(original)
                  if line.startswith(gate["function"])]
        if not starts:
            raise ExplainError(
                f"could not find {gate['function']!r} in {path}; the gate "
                "definition in this script needs updating"
            )
        begin, end = starts[0], brace_range(original, starts[0])
    else:
        begin, end = 0, len(original) - 1

    helper = [
        "namespace {",
        "// Inserted by scripts/debug/explain_rejection.py; reverted after the run.",
        "bool __gtiExplainReject(int line) {",
        f'  static const bool on = std::getenv("{gate["environment"]}") != nullptr;',
        "  if (on) {",
        f'    std::fprintf(stderr, "{gate["marker"]} %d\\n", line);',
        "  }",
        "  return false;",
        "}",
        "} // namespace",
    ]

    # Build the output as (text, original-1-based-line-or-None) pairs so the
    # mapping is exact by construction rather than derived from offsets.
    emitted: list[tuple[str, int | None]] = [
        ("#include <cstdio>", None),
        ("#include <cstdlib>", None),
    ]
    count = 0
    for index, line in enumerate(original):
        if index == begin:
            emitted.extend((text, None) for text in helper)
        within = begin <= index <= end
        indent = line[: len(line) - len(line.lstrip())]
        if within and gate["pattern"] == "return false;" \
                and line.strip() == "return false;":
            emitted.append((f"{indent}return __gtiExplainReject(__LINE__);",
                            index + 1))
            count += 1
        elif within and gate["pattern"] != "return false;" \
                and gate["pattern"] in line:
            emitted.append((f"{indent}(void)__gtiExplainReject(__LINE__);", None))
            emitted.append((line, index + 1))
            count += 1
        else:
            emitted.append((line, index + 1))

    mapping = {number: origin
               for number, (_, origin) in enumerate(emitted, start=1)
               if origin is not None}
    path.write_text("\n".join(text for text, _ in emitted))
    return count, mapping, original


def guarding_comment(lines: list[str], line_number: int) -> tuple[str, str, bool]:
    """The rejecting statement, its nearest preceding comment, and whether it
    sits inside a speculative lambda."""
    index = line_number - 1
    if not 0 <= index < len(lines):
        return "?", "", False
    statement = lines[index].strip()

    # A rejection inside `[&]() { ... }` is a shape probe, not a decision.
    # Walk outward through every enclosing block, not just the innermost:
    # the rejection usually sits inside an `if` inside the lambda, so stopping
    # at the first opener would always miss the lambda introducer.
    probe = False
    depth = 0
    introducer = re.compile(r"\[[&=][^\]]*\]\s*\([^)]*\)")
    for back in range(index, max(index - 400, -1), -1):
        depth += lines[back].count("}") - lines[back].count("{")
        if depth < 0:
            if introducer.search(lines[back]):
                probe = True
                break
            depth = 0  # leave this block and keep looking outward

    comment: list[str] = []
    cursor = index - 1
    while cursor >= 0 and cursor > index - 40:
        if lines[cursor].strip().startswith("//"):
            while cursor >= 0 and lines[cursor].strip().startswith("//"):
                comment.insert(0, lines[cursor].strip().lstrip("/ ").rstrip())
                cursor -= 1
            break
        cursor -= 1
    return statement, " ".join(comment), probe


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Name the compiler gate that rejected a program.")
    parser.add_argument("--source", required=True,
                        help="the .gti reproducer that raises GTI-B0001")
    parser.add_argument("--gate", choices=sorted(GATES), default="emitter")
    parser.add_argument("--build", default="build",
                        help="build directory to rebuild into (default: build)")
    parser.add_argument("--keep", action="store_true",
                        help="leave the instrumented compiler in place")
    arguments = parser.parse_args()

    gate = GATES[arguments.gate]
    target = REPOSITORY_ROOT / gate["file"]
    if not target.is_file():
        raise ExplainError(f"{target} not found")
    source = Path(arguments.source).resolve()
    if not source.is_file():
        raise ExplainError(f"{source} not found")
    build = (REPOSITORY_ROOT / arguments.build).resolve()

    backup = Path(tempfile.mkdtemp(prefix="gti-explain-")) / target.name
    shutil.copy2(target, backup)
    print(f"instrumenting {gate['file']} (original saved to {backup})")

    try:
        count, mapping, original_lines = instrument(target, gate)
        print(f"  {count} rejection points instrumented; rebuilding...")
        built = subprocess.run(
            ["cmake", "--build", str(build), "--target", "gti"],
            capture_output=True, text=True, timeout=3600)
        if built.returncode != 0:
            print(built.stdout[-3000:] + built.stderr[-3000:], file=sys.stderr)
            raise ExplainError("instrumented build failed")

        environment = dict(os.environ)
        environment[gate["environment"]] = "1"
        run = subprocess.run(
            [str(build / "gti"), "--emit-cpp", str(source), "-o", os.devnull],
            capture_output=True, text=True, timeout=600, env=environment)

        # Translate instrumented line numbers back to the pristine file.
        hits: dict[int, int] = {}
        for match in re.finditer(rf"{gate['marker']} (\d+)", run.stderr):
            reported = int(match.group(1))
            line = mapping.get(reported, reported)
            hits[line] = hits.get(line, 0) + 1

        print()
        if not hits:
            print(f"No {arguments.gate} gate fired. The rejection is in a "
                  "different subsystem — try --gate "
                  f"{'verifier' if arguments.gate == 'emitter' else 'emitter'}.")
        else:
            print(f"{len(hits)} distinct {arguments.gate} gate(s) fired:\n")
            for line, times in sorted(hits.items()):
                statement, comment, probe = guarding_comment(
                    original_lines, line)
                label = "  (probe — likely not the cause)" if probe else ""
                print(f"  {gate['file']}:{line}  hit {times}x{label}")
                if comment:
                    print(f"      {comment[:400]}")
                print()
        # The compiler's own message names the failing body.
        for text in run.stderr.split("\n"):
            if "GTI-B0001" in text:
                print("compiler diagnostic:")
                print(f"  {text.strip()[:400]}")
                break
    finally:
        if arguments.keep:
            print(f"\n--keep: instrumented source left in place; restore with\n"
                  f"  cp {backup} {target}")
        else:
            shutil.copy2(backup, target)
            # copy2 preserves the backup's mtime, which predates the build
            # outputs — ninja would then skip the rebuild and leave the
            # instrumented binary in place despite a byte-perfect restore.
            os.utime(target, None)
            restored = target.read_text() == backup.read_text()
            print(f"\nrestored {gate['file']}: "
                  f"{'verified identical' if restored else 'MISMATCH — restore by hand'}")
            if not restored:
                raise ExplainError(
                    f"restore verification failed; copy {backup} over {target}")
            print("rebuild to get a clean compiler: "
                  f"cmake --build {arguments.build} --target gti")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ExplainError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
