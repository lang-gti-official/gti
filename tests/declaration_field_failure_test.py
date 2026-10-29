#!/usr/bin/env python3
"""Source and generated constructors: exact field publication/failure unwind."""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run
from allocation_abort_runtime_test import ABORT_STATUS


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "declaration_field_failure.gti"
    lines = source.read_text().splitlines()
    constructor_site = next(i for i, line in enumerate(lines, 1)
                            if "constructor_failure =" in line)
    body_site = next(i for i, line in enumerate(lines, 1) if "body_failure =" in line)
    generated_site = next(i for i, line in enumerate(lines, 1)
                          if "generated_failure =" in line)
    cleanup_site = next(i for i, line in enumerate(lines, 1)
                        if "generated_cleanup_failure =" in line)
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    allocation_site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    allocation = (f"<std/detail/memory_core>:{allocation_site}: runtime error[GTI-R0011]: "
                  "allocation_failure in unique_owner\n")
    division = lambda site: (f"{source.name}:{site}: runtime error[GTI-R0002]: "
                             "division_by_zero in integer_division\n")
    # Completed fields unwind before the constructor parameter, which unwinds
    # before the caller. A failed Resource construction never drops Resource.
    # Exhaustion is different: it aborts before any constructor rollback.
    cases = [(0, "8\n10\n2\n1\n9\n", ""),
             (ABORT_STATUS, "", allocation),
             (ABORT_STATUS, "", allocation),
             (70, "1\n8\n9\n", division(constructor_site)),
             (70, "2\n1\n8\n9\n", division(body_site)),
             (0, "3\n1\n9\n", ""),
             (0, "11\n9\n", ""),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (70, "1\n11\n9\n", division(generated_site)),
             (70, "1\n11\n9\n", division(generated_site)),
             (70, "5\n1\n11\n15\n9\n", division(generated_site)),
             (70, "1\n11\n",
              f"{source.name}:{cleanup_site}: runtime error[GTI-R0014]: "
              "failure_during_cleanup; primary [GTI-R0002] at "
              f"{source.name}:{generated_site}; secondary [GTI-R0002]\n"),
             (70, "1\n11\n9\n", division(generated_site)),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (70, "1\n11\n9\n", division(generated_site)),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (70, "1\n11\n9\n", division(generated_site)),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (70, "1\n11\n9\n", division(generated_site)),
             (0, "1\n2\n30\n12\n11\n9\n", ""),
             (70, "1\n11\n9\n", division(generated_site)),
             (70, "9\n", division(generated_site)),
             (0, "2\n30\n12\n9\n", "")]
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
