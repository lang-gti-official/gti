#!/usr/bin/env python3
"""Expected/callback/operator regressions for passive and owning record flows."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("-O0", "-O1", "-O3")]
    modes.append(("c++23", "-O1", ["-fsanitize=address,undefined"]))
    with tempfile.TemporaryDirectory(prefix="gti-passive-records-") as directory:
        for index, (standard, level, native_flags) in enumerate(modes):
            for fixture, output in (("passive_expected_records", "2\n1\n"),
                                    ("passive_callable_records", "17\n"),
                                    ("temporary_operator_records", "5\n6\n2\n1\n")):
                binary = Path(directory) / f"{fixture}-{index}"
                source = fixtures / f"{fixture}.gti"
                built = subprocess.run(
                    [str(compiler), str(source), "--std", standard, level,
                     "--keep-cpp", "-o", str(binary), "--",
                     str(fixtures / "passive_record_oracle.cpp"), *native_flags],
                    capture_output=True, text=True, timeout=scaled(120))
                assert built.returncode == 0, (fixture, standard, level, built.stdout, built.stderr)
                for failure in ([0] if fixture == "passive_callable_records" else [0, 1]):
                    environment = dict(os.environ, GTI_TEST_RECORD_MODE=str(failure),
                                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
                    ran = subprocess.run([str(binary)], env=environment, capture_output=True,
                                         text=True, timeout=scaled(20))
                    if failure:
                        marker = ("late-argument-failure" if fixture == "temporary_operator_records"
                                  else "error-constructor-failure")
                        line = next(i for i, text in enumerate(source.read_text().splitlines(), 1)
                                    if f"// {marker}" in text)
                        failure_output = "7\n2\n1\n" if fixture == "temporary_operator_records" else "2\n1\n"
                        assert ran.returncode == 70 and ran.stdout == failure_output, ran
                        assert ran.stderr == (
                            f"{source.name}:{line}: runtime error[GTI-R0002]: "
                            "division_by_zero in integer_division\n"), ran
                    else:
                        assert (ran.returncode, ran.stdout, ran.stderr) == (0, output, ""), ran
            print(f"{standard} {level} {native_flags}: record regressions passed", flush=True)


if __name__ == "__main__":
    main()
