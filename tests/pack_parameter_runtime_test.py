#!/usr/bin/env python3
"""Owning parameter packs retain explicit normal and failure cleanup."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command):
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=120, env=environment)


def report(fixture, line):
    return (f"{fixture}.gti:{line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")


def secondary(fixture, primary, cleanup):
    return (f"{fixture}.gti:{cleanup}: runtime error[GTI-R0014]: "
            f"failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture}.gti:{primary}; secondary [GTI-R0002]\n")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: pack_parameter_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    original = "pack_parameter_cleanup_failure"
    forwarded = "pack_parameter_lifetimes"
    constructed = "pack_constructor_lifetimes"
    owner = "owner_parameter_pack_cleanup"
    cases = {
        owner: [
            (0, "2\n1\n9\n", ""),
            (70, "2\n1\n9\n", report(owner, 11)),
            (70, "2\n1\n9\n", report(owner, 19)),
            (70, "2\n1\n", secondary(owner, 19, 11)),
        ],
        original: [
            (70, "2\n1\n9\n", report(original, 10)),
            (70, "2\n1\n", secondary(original, 15, 10)),
        ],
        forwarded: [
            (0, "3\n4\n2\n1\n9\n", ""),
            (70, "3\n4\n2\n1\n9\n", report(forwarded, 11)),
            (70, "3\n4\n2\n1\n9\n", report(forwarded, 16)),
            (70, "3\n4\n2\n1\n", secondary(forwarded, 16, 11)),
        ],
        "pack_parameter_copies": [(0, "11\n22\n11\n22\n", "")],
        "pack_parameter_borrows": [(0, "4\n", "")],
        "pack_constructor_copies": [(0, "11\n22\n11\n22\n", "")],
        constructed: [
            (0, "payload\n2\n1\n7\n9\n", ""),
            (70, "payload\n2\n1\n9\n", report(constructed, 11)),
            (70, "2\n1\n9\n", report(constructed, 19)),
            (70, "2\n1\n", secondary(constructed, 19, 11)),
        ],
    }
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("asan-ubsan", ["--std", "c++23", "-O1"]))
    modes.append(("no-elide", ["--std", "c++23", "-O0"]))
    with tempfile.TemporaryDirectory(prefix="gti-pack-parameters-") as temporary:
        for mode, flags in modes:
            for fixture, executions in cases.items():
                binary = pathlib.Path(temporary) / f"{fixture}-{mode}"
                command = [str(compiler), str(fixtures / f"{fixture}.gti"),
                           *flags, "--keep-cpp", "-o", str(binary)]
                if mode == "asan-ubsan":
                    command += ["--", "-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer"]
                elif mode == "no-elide":
                    command += ["--", "-fno-elide-constructors"]
                built = run(command)
                if built.returncode:
                    raise AssertionError(f"{fixture}/{mode} build:\n{built.stdout}{built.stderr}")
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing MIR execution witness")
                for count, expected in enumerate(executions):
                    result = run([str(binary), *(["fail"] * count)])
                    actual = (result.returncode, result.stdout, result.stderr)
                    if actual != expected:
                        raise AssertionError(f"{fixture}/{mode}/{count}: {actual!r} != {expected!r}")
                print(f"{fixture}: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
