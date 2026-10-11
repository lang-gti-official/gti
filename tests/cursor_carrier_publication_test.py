#!/usr/bin/env python3
"""Verify exact physical shell publication and forwarding with retained owners."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    parser.add_argument("--semantic-only", action="store_true")
    options = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixture = repository / "tests/fixtures/cursor_carrier_publication.gti"
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-cursor-publication-") as directory:
        output = pathlib.Path(directory)
        if not options.semantic_only:
            def build(case):
                label, flags, native = case
                executable = output / label
                command = [str(compiler), str(fixture), *flags, "--progress=never",
                           "--keep-cpp", "-o", str(executable)]
                if native:
                    command += ["--", *native]
                result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
                assert result.returncode == 0, (label, result.stdout, result.stderr)
                assert "// GTI verified-MIR body:" in executable.with_suffix(".gti.cpp").read_text()
                return label, executable

            def observe(case):
                label, executable = case
                result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (label, result)
                failed = subprocess.run([str(executable), "failure"], env=environment,
                                        capture_output=True, text=True, timeout=30)
                assert failed.returncode == 70 and failed.stdout == "2\n1\n", (label, failed)
                assert 'runtime error[GTI-R0017]: panic: "invalid user cursor selection"' in failed.stderr, (label, failed)

            modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
            run_cases([(mode[0], mode) for mode in modes], build, options.jobs, observe)

        declarations = fixture.read_text().split("int main()", 1)[0]
        setup = """mut int left = 11; mut int right = 13;
mut UserOwners owners{left, right};
auto cursor = forward_cursor(owners);
"""
        invalid = {
            "first-owner-invalidation": "left = 17; return cursor.read();",
            "second-owner-invalidation": "right = 17; return cursor.read();",
            "physical-shell-movement": "auto moved = std::move(owners); return cursor.read();",
            "readonly-write": "cursor.read() = 17; return 0;",
        }
        for name, body in invalid.items():
            path = output / f"{name}.gti"
            path.write_text(declarations + "int main() {\n" + setup + body + "\n}\n")
            result = subprocess.run([str(compiler), str(path), "--emit-mir", "--progress=never",
                                     "-o", str(output / "invalid.mir")],
                                    env=environment, capture_output=True, text=True, timeout=60)
            assert result.returncode == 65 and "error[GTI-S" in result.stderr, (name, result)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
        print(f"Cursor publication: {len(invalid)} lifetime/access rejections passed", flush=True)


if __name__ == "__main__":
    main()
