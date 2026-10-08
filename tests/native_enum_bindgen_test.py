#!/usr/bin/env python3
"""Verify imported enum ABI against independently compiled C, not emitted C++."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def run(command, cwd=None):
    result = subprocess.run([str(part) for part in command], cwd=cwd,
                            capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    example = root / "examples/native-enum-bindings"
    with tempfile.TemporaryDirectory(prefix="gti-native-enums-") as directory:
        work = Path(directory)
        project = work / "project"
        shutil.copytree(example, project, ignore=shutil.ignore_patterns("build"))
        run([compiler, "build"], cwd=project)
        cached = run([compiler, "build"], cwd=project)
        assert "restoring executable" in cached.stdout + cached.stderr, cached
        run([compiler, "run"], cwd=project)

        header = project / "native/flags.h"
        bindings = work / "bindings.gti"
        run([compiler, "bindgen", header, "--namespace", "native_flags", "-o", bindings])
        original = bindings.read_bytes()
        run([compiler, "bindgen", header, "--namespace", "native_flags", "-o", bindings])
        assert original == bindings.read_bytes(), "enum bindings must be byte-stable"
        source = work / "main.gti"
        source.write_text((project / "src/main.gti").read_text().replace(
            "<flags/bindings>", '"bindings.gti"'))
        modes = [(f"{standard}-{level}", standard, level, [])
                 for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
        modes.append(("sanitized", "c++23", "O1",
                      ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
        for name, standard, level, sanitizers in modes:
            native_object = work / f"native-{name}.o"
            run([os.environ.get("CC", "cc"), "-std=c17", f"-{level}",
                 "-c", project / "native/flags.c", "-o", native_object, *sanitizers])
            binary = work / name
            run([compiler, source, "--std", standard, f"-{level}", "-o", binary,
                 "--", native_object, "-fno-elide-constructors", *sanitizers])
            executed = run([binary])
            assert executed.stdout == "" and executed.stderr == "", executed
            print(f"native enum ABI: {name} passed", flush=True)


if __name__ == "__main__":
    main()
