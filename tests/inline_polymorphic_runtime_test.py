#!/usr/bin/env python3
"""Inline polymorphic owners move, replace, and clean up their payloads.

The fixture uses an application interface and std::function. It counts the
no-throw allocations GTI owners use, so the oracle proves that small payloads
avoid allocation, that boxed fallback still allocates exactly once, and that
no block outlives its owner. Failure phases check that each owned object is
destroyed exactly once. Exhausting the boxed replacement's infallible
allocation reports GTI-R0011 and aborts without running cleanup.
"""

from pathlib import Path
import sys
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS
from unique_class_allocation_runtime_test import run


SUCCESS = """empty empty
inline replace allocations 0
inline inline 3
bumped inline 5
move allocations 0
moved inline 5
other boxed 36
move assign allocations 0
assigned inline 5
~tracked 9
doomed inline 1
boxed replace allocations 1
replaced boxed 7
~tracked 7
back inline inline 11
wide replace allocations 1
wide boxed 8
reset empty
vector 24 16 528
function small allocations 0
function clone allocations 0
function move allocations 0
function calls 5 6
function large allocations 1
function large 10
counter 2
move-only owning allocations 1
move-only 5
transfer small allocations 0
transfer move allocations 0
transfer call 7
transfer large allocations 1
transfer wide 10
transfer once allocations 0
transfer consumed 42
live 0
"""


def line_of(source, text):
    lines = source.read_text().splitlines()
    matches = [index for index, line in enumerate(lines, 1) if text in line]
    assert len(matches) == 1, (text, matches)
    return matches[0]


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "inline_polymorphic_lifecycle.gti"
    native = fixtures / "inline_polymorphic_allocations.cpp"
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    allocation_site = line_of(
        memory, "return unique_ptr<T>(gti_internal::allocate_unique_owner")
    located = lambda text, failure: (
        f"{source.name}:{line_of(source, text)}: runtime error[{failure}\n")
    cases = [
        (0, SUCCESS, ""),
        (ABORT_STATUS, "",
         f"<std/detail/memory_core>:{allocation_site}: runtime error[GTI-R0011]: "
         "allocation_failure in unique_owner\n"),
        (70, "~tracked 5\n",
         located("return value / by;",
                 "GTI-R0002]: division_by_zero in integer_division")),
        (70, "~tracked 6\n",
         located("this.value = this.value + amount;",
                 "GTI-R0001]: integer_overflow in addition")),
        (70, "~tracked 8\n",
         located("return value / zero;",
                 "GTI-R0002]: division_by_zero in integer_division")),
    ]
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-inline-polymorphic-") as temporary:
        for name, flags, native_flags in modes:
            binary = Path(temporary) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(native), *native_flags])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, phase, result.returncode, result.stdout, result.stderr)
            print(f"inline polymorphic lifecycle: {name} passed", flush=True)


if __name__ == "__main__":
    main()
