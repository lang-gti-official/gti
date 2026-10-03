#!/usr/bin/env python3
"""Carrier references retain shell and external lifetimes through cleanup."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def run(command):
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=120, env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixture = pathlib.Path(__file__).parent / "fixtures/carrier_reference_lifetimes.gti"
    original = fixture.read_text()

    def failure(marker, code, detail):
        line = next(index for index, text in enumerate(original.splitlines(), 1)
                    if marker in text)
        return f"carrier_reference_lifetimes.gti:{line}: runtime error[{code}]: {detail}\n"

    cases = [
        ("success", original, (0, "90\n90\n90\n6\n999\n", "")),
        ("overflow", original.replace("Counter owner{4}", "Counter owner{2147483647}"),
         (70, "90\n90\n999\n", failure("// checked addition", "GTI-R0001",
                                    "integer_overflow in addition"))),
        ("return-cleanup", original.replace("holder.get(1).advance()", "holder.get(0).advance()"),
         (70, "90\n90\n999\n", failure("// cleanup failure", "GTI-R0002",
                                    "division_by_zero in integer_division"))),
        ("copy-return-cleanup", original.replace("copied_return(shared, 1)",
                                                "copied_return(shared, 0)"),
         (70, "90\n999\n", failure("// cleanup failure", "GTI-R0002",
                                    "division_by_zero in integer_division"))),
    ]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("asan-ubsan", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-carrier-reference-") as temporary:
        root = pathlib.Path(temporary)
        source = root / fixture.name
        for mode, flags, native in modes:
            for name, text, expected in cases:
                source.write_text(text)
                binary = root / f"{mode}-{name}"
                command = [str(compiler), str(source), *flags, "-o", str(binary)]
                if native:
                    command += ["--", *native]
                built = run(command)
                if built.returncode:
                    raise AssertionError(f"{mode}/{name} build:\n{built.stdout}{built.stderr}")
                executed = run([str(binary)])
                actual = (executed.returncode, executed.stdout, executed.stderr)
                if actual != expected:
                    raise AssertionError(f"{mode}/{name}: {actual!r} != {expected!r}")
            print(f"carrier references: {mode} passed ({len(cases)} outcomes)", flush=True)


if __name__ == "__main__":
    main()
