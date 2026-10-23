#!/usr/bin/env python3
"""Expression text is literal-backed metadata with no argument execution."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parents[1]
    source = repository / "tests" / "fixtures" / "expression_text.gti"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))
    modes = [(standard, level, []) for standard in (20, 23) for level in (0, 1, 3)]
    modes += [(23, 1, ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-expression-text-") as temporary:
        for index, (standard, level, native) in enumerate(modes):
            executable = pathlib.Path(temporary) / f"text-{index}"
            command = [str(args.compiler.resolve()), str(source), "--std",
                       f"c++{standard}", f"-O{level}", "--keep-cpp", "-o",
                       str(executable), "--", *native]
            built = subprocess.run(command, env=environment, capture_output=True,
                                   text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            cpp = executable.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in cpp
            ran = subprocess.run([str(executable)], env=environment,
                                 capture_output=True, text=True, timeout=20)
            assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", ""), (
                standard, level, native, ran.returncode, ran.stdout, ran.stderr)
            print(f"c++{standard} O{level} {native}: expression text passed", flush=True)


if __name__ == "__main__":
    main()
