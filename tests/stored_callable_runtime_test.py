#!/usr/bin/env python3
"""Stored exact callables retain MIR invocation and capture cleanup."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command):
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=120, env=environment)


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    failure = fixtures / "stored_callable_failure.gti"
    lines = failure.read_text().splitlines()
    cleanup = next(i + 1 for i, line in enumerate(lines) if "1 / this.cleanup_divisor" in line)
    # Match the fixture's dispatch order independently of declaration order.
    bodies = []
    for function in ("stored", "direct", "heap_stored", "erased_stored"):
        start = next(i for i, line in enumerate(lines)
                     if line.startswith(f"int {function}("))
        bodies.append(next(i + 1 for i in range(start, len(lines))
                           if "token.read() / divisor" in lines[i]))
    def report(line):
        return f"{failure.name}:{line}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"

    def double_failure(primary):
        return (f"{failure.name}:{cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{failure.name}:{primary}; secondary [GTI-R0002]\n")

    executions = []
    for body in bodies:
        executions += [
            (0, "1\n2\n9\n", ""),
            (70, "1\n2\n9\n", report(cleanup)),
            (70, "1\n2\n9\n", report(body)),
            (70, "1\n", double_failure(body)),
            (70, "1\n2\n9\n", report(cleanup)),
            (70, "1\n2\n", double_failure(body)),
            (70, "1\n2\n", double_failure(cleanup)),
        ]
    pack = fixtures / "stored_callable_pack.gti"
    pack_lines = pack.read_text().splitlines()
    pack_cleanup = next(i + 1 for i, line in enumerate(pack_lines)
                        if "1 / this.cleanup_divisor" in line)
    pack_body = next(i + 1 for i, line in enumerate(pack_lines)
                     if "1 / divisor" in line)
    def pack_report(line):
        return f"{pack.name}:{line}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"

    def pack_double(primary):
        return (f"{pack.name}:{pack_cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{pack.name}:{primary}; secondary [GTI-R0002]\n")

    pack_executions = [
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", pack_report(pack_cleanup)),
        (70, "2\n1\n9\n", pack_report(pack_body)),
        (70, "2\n", pack_double(pack_body)),
        (70, "2\n1\n", pack_double(pack_cleanup)),
    ]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("asan-ubsan", ["--std", "c++23", "-O1"], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-stored-callable-") as temporary:
        for mode, flags, native in modes:
            for source, cases in [(fixtures / "stored_callable_invocation.gti", [(0, "", "")]),
                                  (fixtures / "stored_callable_source_owner.gti", [(0, "8\n7\n", "")]),
                                  (failure, executions), (pack, pack_executions)]:
                binary = pathlib.Path(temporary) / f"{source.stem}-{mode}"
                command = [str(compiler), str(source), *flags, "-o", str(binary)]
                if native:
                    command += ["--", *native]
                built = run(command)
                assert built.returncode == 0, f"{source.name}/{mode}: {built.stdout}{built.stderr}"
                for count, expected in enumerate(cases):
                    result = run([str(binary), *(["fail"] * count)])
                    actual = result.returncode, result.stdout, result.stderr
                    assert actual == expected, f"{source.name}/{mode}/{count}: {actual!r} != {expected!r}"
            print(f"stored callables: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
