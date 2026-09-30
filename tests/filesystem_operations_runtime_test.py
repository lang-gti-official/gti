#!/usr/bin/env python3
"""Exercise host filesystem workflows and symlink boundaries in isolated roots."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    configurations = {
        f"{standard}-{level}": (["--std", standard, f"-{level}"], [])
        for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")
    }
    configurations["no-elide"] = (["--std", "c++23", "-O0"], ["-fno-elide-constructors"])
    configurations["sanitized"] = (["--std", "c++23", "-O1"],
                                   ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append",
                        choices=list(configurations))
    parser.add_argument("--native-source", type=pathlib.Path)
    args = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = args.mode or list(configurations)
    with tempfile.TemporaryDirectory(prefix="gti-filesystem-operations-") as temporary:
        for mode in modes:
            flags, native_flags = configurations[mode]
            work = pathlib.Path(temporary) / mode
            work.mkdir()
            executable = work / "program"
            command = [str(args.compiler.resolve()),
                       str(repository / "tests/fixtures/filesystem_operations.gti"),
                       *flags, "-o", str(executable)]
            native_inputs = ([str(args.native_source.resolve())] if args.native_source else [])
            if native_flags or native_inputs:
                command += ["--", *native_flags, *native_inputs]
            build = subprocess.run(command, env=environment, capture_output=True,
                                   text=True, timeout=300)
            assert build.returncode == 0, (mode, build.stdout, build.stderr)
            run = subprocess.run([str(executable)], cwd=work, env=environment,
                                 capture_output=True, text=True, timeout=60)
            assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), (mode, run)
            assert not (work / "fs-work").exists()
            assert not (work / "fs-keep").exists()
            print(f"filesystem operations: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
