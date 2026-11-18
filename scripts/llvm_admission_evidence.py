#!/usr/bin/env python3
"""Capture and compare RFC 0007 whole-plan/LLVM evidence, never bless changes.

The capture command defaults to the single-file root examples. Supply an
explicit input manifest (a JSON array of repository-relative source paths) for
other ordinary GTI sources. Multi-section UI tests need materializing first;
this tool does not pretend their embedded files are ordinary source files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


EQUIVALENCE_FILES = ("plan.txt", "outcome.txt", "module.ll", "module.o")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def capture(args: argparse.Namespace) -> int:
    root, runner = args.root.resolve(), args.runner.resolve()
    names = (json.loads(args.inputs.read_text()) if args.inputs else
             [str(p.relative_to(root)) for p in
              sorted((root / "examples").glob("*.gti"))])
    if (not isinstance(names, list) or not names or
            any(not isinstance(name, str) for name in names) or
            len(set(names)) != len(names)):
        raise ValueError("input manifest must be a nonempty list of unique paths")
    sources = []
    for name in names:
        path = root / name
        if Path(name).is_absolute() or not path.resolve().is_relative_to(root):
            raise ValueError(f"input escapes source root: {name}")
        if not path.is_file():
            raise ValueError(f"missing source: {name}")
        sources.append((name, path, digest(path)))
    args.output.mkdir(parents=True, exist_ok=False)
    stdlib = {str(p.relative_to(root)): digest(p) for p in
              sorted((root / "stdlib").rglob("*")) if p.is_file()}
    if not stdlib:
        raise ValueError("source root has no standard library")
    defaults = {}
    for _, source, _ in sources:
        for directory in source.parents:
            path = directory / "gti-test.defaults"
            if path.is_file():
                defaults[str(path)] = digest(path)
    report = {"schema": 1, "source_root": str(root),
              "platform": [platform.system(), platform.machine()],
              "runner": str(runner), "runner_sha256": digest(runner),
              "validation": args.validation, "stdlib": stdlib,
              "defaults": defaults,
              "sources": {name: sha for name, _, sha in sources},
              "complete": False, "cases": {}}
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       GTI_COMPILER_VALIDATION="1" if args.validation else "0")
    failed = False
    for name, source, sha in sources:
        for level in ("O0", "O2"):
            identity = name + ":" + level
            relative = hashlib.sha256(identity.encode()).hexdigest()[:24]
            directory = args.output / relative
            argv = [str(runner), "--llvm-admission-evidence", str(source),
                    str(directory.resolve()), level]
            try:
                result = subprocess.run(argv, env=environment, cwd=root,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=args.timeout)
                code, log = result.returncode, result.stdout
            except subprocess.TimeoutExpired as error:
                code, log = -1, (error.stdout or b"") + b"\ncapture timed out\n"
            (args.output / (relative + ".log")).write_bytes(log)
            files = {filename: {"sha256": digest(directory / filename),
                                "size": (directory / filename).stat().st_size}
                     for filename in EQUIVALENCE_FILES
                     if (directory / filename).is_file()}
            valid = code == 0 and "outcome.txt" in files
            if valid and (directory / "outcome.txt").read_text() == "admitted\n":
                valid = all(filename in files for filename in EQUIVALENCE_FILES)
            if args.validation and "plan.txt" in files:
                stats = directory / "statistics.txt"
                valid = (valid and stats.is_file() and
                         stats.read_text().startswith("llvm-admission-statistics-v1\n"))
            report["cases"][identity] = {"directory": relative, "exit": code,
                                         "valid": valid, "files": files}
            failed |= not valid
            save(args.output / "manifest.json", report)
            print(f"{'captured' if valid else 'FAILED'} {identity}", flush=True)
    # Changing inputs during a long corpus capture is not valid evidence.
    unchanged = all(digest(path) == sha for _, path, sha in sources)
    unchanged &= digest(runner) == report["runner_sha256"]
    unchanged &= all(digest(root / name) == sha for name, sha in stdlib.items())
    unchanged &= all(digest(Path(name)) == sha for name, sha in defaults.items())
    report["complete"] = not failed and unchanged
    save(args.output / "manifest.json", report)
    return 0 if report["complete"] else 1


def compare(before: Path, after: Path) -> list[str]:
    left = json.loads((before / "manifest.json").read_text())
    right = json.loads((after / "manifest.json").read_text())
    differences = []
    for field in ("schema", "source_root", "platform", "stdlib", "defaults", "sources"):
        if left.get(field) != right.get(field):
            differences.append(f"incomparable {field}")
    if not left.get("complete") or not right.get("complete"):
        differences.append("capture incomplete")
    for record in (left, right):
        expected = {name + ":" + level for name in record["sources"]
                    for level in ("O0", "O2")}
        if not expected or set(record["cases"]) != expected:
            differences.append("capture does not cover its complete input inventory")
    if left["cases"].keys() != right["cases"].keys():
        differences.append("case inventory differs")
    for name in sorted(left["cases"].keys() & right["cases"].keys()):
        a, b = left["cases"][name], right["cases"][name]
        if not a["valid"] or not b["valid"] or a["exit"] != 0 or b["exit"] != 0:
            differences.append(f"{name}: unsuccessful capture")
        if a["files"].keys() != b["files"].keys():
            differences.append(f"{name}: artifact inventory differs")
        for filename in sorted(a["files"].keys() & b["files"].keys()):
            # Compare actual retained bytes, not just two claimed hash lists.
            pa, pb = before / a["directory"] / filename, after / b["directory"] / filename
            if digest(pa) != a["files"][filename]["sha256"] or digest(pb) != b["files"][filename]["sha256"]:
                differences.append(f"{name}: {filename} evidence changed since capture")
            if pa.read_bytes() != pb.read_bytes():
                differences.append(f"{name}: {filename} differs")
    return differences


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture")
    cap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    cap.add_argument("--runner", type=Path, required=True)
    cap.add_argument("--output", type=Path, required=True)
    cap.add_argument("--inputs", type=Path)
    cap.add_argument("--validation", action="store_true")
    cap.add_argument("--timeout", type=float, default=240)
    diff = commands.add_parser("compare")
    diff.add_argument("before", type=Path)
    diff.add_argument("after", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "capture":
            return capture(args)
        differences = compare(args.before, args.after)
        for difference in differences:
            print(difference)
        if not differences:
            print("All admission plans, outcomes, LLVM IR and objects are byte-identical.")
        return int(bool(differences))
    except (OSError, ValueError, KeyError) as error:
        print(f"admission evidence: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
