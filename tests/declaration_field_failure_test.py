#!/usr/bin/env python3
"""Source constructor field defaults: exact publication and failure unwind."""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "declaration_field_failure.gti"
    lines = source.read_text().splitlines()
    constructor_site = next(i for i, line in enumerate(lines, 1)
                            if "constructor_failure =" in line)
    body_site = next(i for i, line in enumerate(lines, 1) if "body_failure =" in line)
    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
    allocation_site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    allocation = (f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                  "allocation_failure in unique_owner\n")
    division = lambda site: (f"{source.name}:{site}: runtime error[GTI-R0002]: "
                             "division_by_zero in integer_division\n")
    # Completed fields unwind before the constructor parameter, which unwinds
    # before the caller. A failed Resource construction never drops Resource.
    cases = [(0, "8\n10\n2\n1\n9\n", ""),
             (70, "8\n9\n", allocation),
             (70, "1\n8\n9\n", allocation),
             (70, "1\n8\n9\n", division(constructor_site)),
             (70, "2\n1\n8\n9\n", division(body_site)),
             (0, "3\n1\n9\n", ""),
             (0, "11\n9\n", "")]
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-field-failure-") as directory:
        for name, flags, native in modes:
            binary = Path(directory) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(fixtures / "unique_scalar_allocation_fault.cpp"), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, phase, result.returncode, result.stdout, result.stderr, expected)
            print(f"declaration-field failure: {name} passed", flush=True)


if __name__ == "__main__":
    main()
