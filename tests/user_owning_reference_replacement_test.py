#!/usr/bin/env python3
"""Owning call results replace checked referents in the MIR-owned schedule."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


MODES = {
    f"{standard}-{level}": ["--std", standard, f"-{level}"]
    for standard in ("c++20", "c++23")
    for level in ("O0", "O1", "O3")
}
MODES["no-elide"] = ["--std", "c++23", "-O3", "--", "-fno-elide-constructors"]
MODES["sanitized"] = [
    "--std", "c++23", "-O1", "--", "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
]


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


def check(compiler, repository, directory, mode):
    flags = MODES[mode]
    for fixture in ("user_owning_reference_replacement",
                    "user_owning_reference_replacement_failure"):
        source = repository / "tests" / "fixtures" / f"{fixture}.gti"
        binary = directory / f"{fixture}-{mode}"
        built = run([str(compiler), str(source), "--keep-cpp", "--progress=never",
                     "-o", str(binary), *flags])
        assert built.returncode == 0, (mode, fixture, built.stdout, built.stderr)
        assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
        completed = run([str(binary)])
        expected_output = "100\n200\n10\n20\n" if fixture.endswith("failure") else ""
        assert completed.returncode == 0 and completed.stdout == expected_output
        assert completed.stderr == "", (mode, fixture, completed)
        if fixture.endswith("failure"):
            for arguments, output, reason in (
                (["fail"], "100\n10\n", "accessor failed"),
                (["fail", "fail"], "100\n200\n10\n", "replacement failed"),
                (["fail", "fail", "fail"], "100\n200\n20\n10\n",
                 "replacement partially built"),
            ):
                failed = run([str(binary), *arguments])
                assert failed.returncode == 70 and failed.stdout == output, (
                    mode, arguments, failed,
                )
                assert reason in failed.stderr, (mode, arguments, failed.stderr)
                assert "runtime error[GTI-R0017]" in failed.stderr
                assert "GTI-R0014" not in failed.stderr
                assert "AddressSanitizer" not in failed.stderr
                assert "runtime error:" not in failed.stderr
    return mode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append", choices=MODES)
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    args = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    os.environ["ASAN_OPTIONS"] = "halt_on_error=1"
    os.environ["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    compiler = args.compiler.resolve()
    modes = args.mode or list(MODES)
    with tempfile.TemporaryDirectory(prefix="gti-owning-reference-") as temporary:
        directory = pathlib.Path(temporary)
        run_cases([(mode, mode) for mode in modes],
                  lambda mode: check(compiler, repository, directory, mode),
                  args.jobs)


if __name__ == "__main__":
    main()
