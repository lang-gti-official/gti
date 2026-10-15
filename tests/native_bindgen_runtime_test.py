#!/usr/bin/env python3
"""Verify generated native bindings against independently compiled C."""

import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


def run(command, cwd=None):
    result = subprocess.run([str(part) for part in command], cwd=cwd,
                            capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    cases = {"native-enum-bindings": ("flags", "native_flags"),
             "native-callback-slots": ("hooks", "native"),
             "native-union-bindings": ("events", "native")}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--example", choices=cases, default="native-enum-bindings")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    binding, namespace = cases[arguments.example]
    root = Path(__file__).resolve().parent.parent
    example = root / "examples" / arguments.example
    with tempfile.TemporaryDirectory(prefix="gti-native-bindings-") as directory:
        work = Path(directory)
        project = work / "project"
        shutil.copytree(example, project, ignore=shutil.ignore_patterns("build"))
        run([compiler, "build"], cwd=project)
        cached = run([compiler, "build"], cwd=project)
        assert "restoring executable" in cached.stdout + cached.stderr, cached
        run([compiler, "run"], cwd=project)

        header = project / "native" / f"{binding}.h"
        bindings = work / "bindings.gti"
        run([compiler, "bindgen", header, "--namespace", namespace, "-o", bindings])
        original = bindings.read_bytes()
        run([compiler, "bindgen", header, "--namespace", namespace, "-o", bindings])
        assert original == bindings.read_bytes(), "native bindings must be byte-stable"
        source = work / "main.gti"
        source.write_text((project / "src/main.gti").read_text().replace(
            f"<{binding}/bindings>", '"bindings.gti"'))
        modes = [(f"{standard}-{level}", standard, level, [])
                 for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
        modes.append(("sanitized", "c++23", "O1",
                      ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
        for name, standard, level, sanitizers in modes:
            native_object = work / f"native-{name}.o"
            run([os.environ.get("CC", "cc"), "-std=c17", f"-{level}",
                 "-c", project / "native" / f"{binding}.c", "-o", native_object, *sanitizers])
            binary = work / name
            run([compiler, source, "--std", standard, f"-{level}", "-o", binary,
                 "--", native_object, "-fno-elide-constructors", *sanitizers])
            executed = run([binary])
            assert executed.stdout == "" and executed.stderr == "", executed
            print(f"{arguments.example}: {name} passed", flush=True)


if __name__ == "__main__":
    main()
