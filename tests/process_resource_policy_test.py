#!/usr/bin/env python3
"""Source provider policy, native forwarding, cleanup and optional-audit parity."""

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile


def run(command, environment, expected_status=0):
    result = subprocess.run(command, env=environment, capture_output=True,
                            text=True, timeout=180)
    assert result.returncode == expected_status, (command, result.returncode, result.stdout, result.stderr)
    return result


def installed_resources(compiler):
    """Pin every resource to the selected installation; forbid build fallbacks."""
    prefix = pathlib.Path(compiler).resolve().parent.parent
    libraries = [prefix / "lib" / name
                 for name in ("libgti_runtime.a", "gti_runtime.lib")]
    libraries = [path for path in libraries if path.is_file()]
    if len(libraries) != 1:
        raise RuntimeError(f"expected one installed runtime library under {prefix / 'lib'}")
    resources = {
        "GTI_STDLIB_PATH": prefix / "share/gti/stdlib",
        "GTI_RUNTIME_INCLUDE": prefix / "include",
        "GTI_RUNTIME_LIBRARY": libraries[0],
    }
    for required in (resources["GTI_STDLIB_PATH"] / "prelude.gti",
                     resources["GTI_STDLIB_PATH"] / "std/memory.gti",
                     resources["GTI_RUNTIME_INCLUDE"] / "gti/runtime.hpp",
                     resources["GTI_RUNTIME_INCLUDE"] / "gti/memory.h"):
        if not required.is_file():
            raise RuntimeError(f"missing installed resource: {required}")
    return {name: str(path) for name, path in resources.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--installed", action="store_true",
                        help="Use only the staged toolchain's library and runtime resources.")
    parser.add_argument("--mode", action="append",
                        help="Run selected matrix mode(s); the default runs all eight.")
    arguments = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    compiler = shutil.which(str(arguments.compiler)) or str(arguments.compiler.resolve())
    environment = dict(os.environ,
                       ASAN_OPTIONS="halt_on_error=1:detect_leaks=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    if arguments.installed:
        # Installed evidence must ignore build-tree resource overrides. In
        # build-tree mode, CTest supplies the exact runtime archive; release
        # compilers deliberately have no embedded source/build fallback.
        for name in ("GTI_STDLIB_PATH", "GTI_RUNTIME_INCLUDE", "GTI_RUNTIME_LIBRARY"):
            environment.pop(name, None)
        environment.update(installed_resources(compiler))
    else:
        environment.update(GTI_STDLIB_PATH=str(root / "stdlib"),
                           GTI_RUNTIME_INCLUDE=str(root / "runtime/include"))
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
    cleanup = root / "tests/fixtures/process_storage_cleanup.gti"
    cleanup_lines = cleanup.read_text().splitlines()

    def failure(expression):
        line = next(index for index, text in enumerate(cleanup_lines, 1)
                    if expression in text)
        return (f"{cleanup.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    with tempfile.TemporaryDirectory(prefix="gti-process-provider-") as directory:
        temporary = pathlib.Path(directory)
        # Fault injection copies the selected library (including in installed
        # mode), never a build-tree fallback. Ordinary cases below still execute
        # the unchanged selected library. Only this extra case narrows its bound.
        limited_library = temporary / "limited-stdlib"
        shutil.copytree(environment["GTI_STDLIB_PATH"], limited_library)
        memory = limited_library / "std/memory.gti"
        original = memory.read_text()
        needle = "uint64_t size_bytes = std::layout_of<c_size_t>().bytes;"
        assert original.count(needle) == 1, "native-size fault injection seam changed"
        memory.write_text(original.replace(needle, "uint64_t size_bytes = uint64_t(1);"))
        limited_environment = dict(environment, GTI_STDLIB_PATH=str(limited_library))
        for label, flags, native_flags in modes:
            for fixture, native in (
                ("process_resource_policy", root / "tests/fixtures/process_resource_policy_native.cpp"),
                ("process_storage_policy", root / "tests/fixtures/process_resource_policy_native.cpp"),
                ("raw_resource_movement", None),
            ):
                binary = temporary / f"{fixture}-{label}"
                native_inputs = [str(native)] if native else []
                run([compiler, str(root / "tests/fixtures" / f"{fixture}.gti"),
                     *flags, "-o", str(binary), "--", *native_inputs,
                     *native_flags], environment)
                result = run([str(binary)], environment)
                assert not result.stdout and not result.stderr, result
            binary = temporary / f"process-resource-limit-{label}"
            run([compiler, str(root / "tests/fixtures/process_resource_policy_limit.gti"),
                 *flags, "-o", str(binary), "--",
                 str(root / "tests/fixtures/process_resource_policy_native.cpp"),
                 *native_flags], limited_environment)
            result = run([str(binary)], limited_environment)
            assert not result.stdout and not result.stderr, result
            binary = temporary / f"process-storage-cleanup-{label}"
            run([compiler, str(cleanup), *flags, "-o", str(binary), "--",
                 str(root / "tests/fixtures/process_resource_policy_native.cpp"),
                 *native_flags], environment)
            for case_arguments, status, output, error in (
                ([], 0, "2\n2\n20\n120\n10\n110\n-2\n-1\n", ""),
                (["constructor"], 70, "2\n2\n120\n10\n110\n-2\n-1\n",
                 failure("1 / construct_divisor")),
                (["destructor", "failure"], 70, "2\n2\n20\n120\n10\n110\n-2\n-1\n",
                 failure("1 / this.destroy_divisor")),
            ):
                result = run([str(binary), *case_arguments], environment, status)
                assert (result.stdout, result.stderr) == (output, error), result
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
