#!/usr/bin/env python3
"""Storage swaps: exact values/capacity, zero allocations, cleanup and loans."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from native_fixture_build import build_fixture

def run(command):
    environment = os.environ.copy()
    for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        previous = environment.get(option, "")
        environment[option] = (previous + ":" if previous else "") + "halt_on_error=1"
    return subprocess.run(list(map(str, command)), capture_output=True, text=True,
                          timeout=120, env=environment)


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-container-swap-") as directory:
        root = Path(directory)
        for mode, flags, native_flags in modes:
            for fixture in ("container_swap", "container_swap_cleanup"):
                binary = root / f"{fixture}-{mode}"
                native = [fixtures / "container_swap_oracle.cpp"] if fixture == "container_swap" else []
                command = [compiler, fixtures / f"{fixture}.gti", *flags]
                if not native:
                    command.append("--keep-cpp")
                environment = os.environ.copy()
                for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
                    previous = environment.get(option, "")
                    environment[option] = (previous + ":" if previous else "") + "halt_on_error=1"
                built = build_fixture(
                    command, binary, native_sources=native, native_arguments=native_flags,
                    standard=flags[1], optimization=flags[2][1:],
                    capture_output=True, text=True, timeout=120, env=environment)
                assert built.returncode == 0, (mode, fixture, built.stdout, built.stderr)
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated
                # Runtime support may contain its own primitive swaps. The container
                # operations must be emitted from verified GTI bodies, without
                # delegating those bodies to the host standard-library swap.
                bodies = generated.split("// GTI verified-MIR body:", 1)[1]
                assert "std::swap(" not in bodies
                executions = ([], ["fail"]) if fixture.endswith("cleanup") else ([],)
                for arguments in executions:
                    result = run([binary, *arguments])
                    expected = "swapped\n5\n4\n6\n2\n1\n3\n" if fixture.endswith("cleanup") else ""
                    assert result.stdout == expected, (mode, fixture, arguments, result)
                    if arguments:
                        assert result.returncode == 70 and re.fullmatch(
                            r".*container_swap_cleanup\.gti:\d+: runtime error"
                            r"\[GTI-R0002\]: division_by_zero in integer_division\n",
                            result.stderr), (mode, result)
                    else:
                        assert result.returncode == 0 and result.stderr == "", (mode, result)
            print(f"container swap: {mode} passed", flush=True)

        for header, spelling, access in (
            ("vector", "std::vector<int32_t>", "a.front()"),
            ("string", "std::string", "a[std::size_t(0)]"),
            ("forward_list", "std::forward_list<int32_t>", "a.front()"),
        ):
            element = "char" if header == "string" else "int32_t"
            for name, setup, expression in (
                ("self", "", "a.swap(a);"),
                ("read-borrow", f"{element}& held = {access};", "a.swap(b); std::println(held);"),
                ("mut-borrow", f"mut {element}& held = {access};", "b.swap(a); std::println(held);"),
                ("immutable", f"{spelling} fixed = {spelling}();", "a.swap(fixed);"),
            ):
                source = root / f"invalid-{header}-{name}.gti"
                source.write_text(f"#include <std/{header}>\nint main() {{\n"
                                  f"mut {spelling} a = {spelling}();\n"
                                  f"mut {spelling} b = {spelling}();\n"
                                  f"{setup}\n{expression}\nreturn 0;\n}}\n")
                artifact = source.with_suffix(".cpp")
                result = run([compiler, source, "--emit-cpp", "-o", artifact])
                assert result.returncode == 65 and "error[GTI-S" in result.stderr, result
                assert "GTI-B0001" not in result.stderr and not artifact.exists(), result


if __name__ == "__main__":
    main()
