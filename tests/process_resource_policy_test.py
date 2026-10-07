#!/usr/bin/env python3
"""Source provider policy, native forwarding, cleanup and optional-audit parity."""

import argparse
import os
import pathlib
import platform
import subprocess
import tempfile


def run(command, environment):
    result = subprocess.run(command, env=environment, capture_output=True,
                            text=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append",
                        help="Run selected matrix mode(s); the default runs all eight.")
    arguments = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    compiler = str(arguments.compiler.resolve())
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(root / "runtime/include"),
                       ASAN_OPTIONS="halt_on_error=1:detect_leaks=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if arguments.mode:
        unknown = set(arguments.mode) - {label for label, _, _ in modes}
        if unknown:
            parser.error(f"unknown modes: {sorted(unknown)}")
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    if platform.system() == "Darwin":
        environment["ASAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="gti-process-provider-") as directory:
        temporary = pathlib.Path(directory)
        for label, flags, native_flags in modes:
            for fixture, native in (
                ("process_resource_policy", root / "tests/fixtures/process_resource_policy_native.cpp"),
                ("raw_resource_movement", root / "runtime/src/memory.cpp"),
            ):
                binary = temporary / f"{fixture}-{label}"
                run([compiler, str(root / "tests/fixtures" / f"{fixture}.gti"),
                     *flags, "-o", str(binary), "--", str(native),
                     f"-I{root / 'runtime/include'}", *native_flags], environment)
                result = run([str(binary)], environment)
                assert not result.stdout and not result.stderr, result
            example = root / "examples/libc-region-provider"
            binary = temporary / f"libc-example-{label}"
            run([compiler, str(example / "src/main.gti"), *flags, "-o", str(binary),
                 "--", str(example / "native/bytes.cpp"), *native_flags], environment)
            result = run([str(binary)], environment)
            assert not result.stdout and not result.stderr, result
            print(f"process provider {label}: passed", flush=True)
        source = root / "tests/fixtures/process_resource_policy.gti"
        emitted = []
        for audit in ("off", "warn"):
            output = temporary / f"audit-{audit}.cpp"
            run([compiler, str(source), "--emit-cpp", "-o", str(output),
                 f"--raw-pointer-audit={audit}"], environment)
            emitted.append(output.read_bytes())
        assert emitted[0] == emitted[1], "optional auditing changed provider code"
        print("process provider Off/Warn parity: passed", flush=True)


if __name__ == "__main__":
    main()
