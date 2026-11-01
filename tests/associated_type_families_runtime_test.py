#!/usr/bin/env python3
"""Execute policy families and their ordinary ownership/failure semantics."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    environment = {**os.environ, "GTI_STDLIB_PATH": str(root / "stdlib")}
    example = root / "examples/182-associated-type-families.gti"
    cleanup = root / "tests/fixtures/associated_type_families.gti"
    aliases = root / "tests/fixtures/associated_type_families_aliases.gti"
    modules = root / "tests/fixtures/associated_type_families_modules"
    failure = output / "failure.gti"
    failure.write_text(cleanup.read_text().replace(
        "std::move(first), 3", "std::move(first), 0"))
    report = {"platform": platform.platform(),
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "sources": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (example, cleanup, aliases, failure,
                                    *sorted(modules.glob("*.gti")))}, "steps": []}

    def run(command, tag, *, expected=0, stdout=None):
        start = time.monotonic()
        result = subprocess.run([str(x) for x in command], capture_output=True,
                                text=True, timeout=180, env=environment)
        log = output / (tag + ".log")
        log.write_text(result.stdout + result.stderr)
        report["steps"].append({"argv": [str(x) for x in command],
                                "exit": result.returncode,
                                "seconds": time.monotonic() - start,
                                "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        assert result.returncode == expected, (tag, result.returncode, expected, log)
        if stdout is not None:
            assert result.stdout == stdout, (tag, result.stdout, stdout, log)
        return result

    rejected = run([compiler, modules / "private_name.gti", "--emit-mir", "-o",
                    output / "private-name.mir"], "private-name", expected=65)
    assert "GTI-S2024" in rejected.stderr and "family_policy::Cell" in rejected.stderr, rejected.stderr

    for backend, standard, level in (("llvm", None, 0), ("llvm", None, 2),
                                     ("llvm", None, 3), ("cpp", "c++20", 2),
                                     ("cpp", "c++23", 2)):
        mode = f"{backend}-{standard or 'native'}-O{level}"
        flags = ["--backend", backend, f"-O{level}"]
        if standard:
            flags += ["--std", standard]
        for source, name, expected, stdout in ((example, "policy", 0, "5\n4\n"),
                                                (cleanup, "cleanup", 0, "1\n2\n"),
                                                (aliases, "aliases", 0, ""),
                                                (modules / "main.gti", "modules", 0, ""),
                                                (failure, "failure", 70, "1\n")):
            binary = output / f"{mode}-{name}"
            run([compiler, source, *flags, "-o", binary], binary.name + "-build")
            result = run([binary], binary.name + "-run", expected=expected, stdout=stdout)
            if name == "failure":
                assert "division by zero" in result.stderr.lower().replace("_", " "), result.stderr
            print(f"{mode} {name}: passed", flush=True)


if __name__ == "__main__":
    main()
