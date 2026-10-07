#!/usr/bin/env python3
"""Empty invocation and allocation failure obey ordinary owning-wrapper policy."""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "owning_function_policy_failure.gti"
    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
    lines = memory.read_text().splitlines()
    read = [i for i, line in enumerate(lines, 1)
            if "return gti_internal::unique_owner_borrow(" in line]
    mutable = [i for i, line in enumerate(lines, 1)
               if "return gti_internal::unique_owner_borrow_mut(" in line]
    assert len(read) == len(mutable) == 2
    allocation_site = next(i for i, line in enumerate(lines, 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    allocation = (f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                  "allocation_failure in unique_owner\n")
    empty = lambda site: (f"<std/memory>:{site}: runtime error[GTI-R0008]: "
                          "empty_owner_access in member_access\n")
    cases = [(70, "9\n", empty(read[1])),
             (70, "9\n", empty(mutable[1])),
             (70, "9\n", empty(read[1])),
             (70, "9\n", empty(mutable[1])),
             (70, "8\n9\n", allocation),
             (70, "9\n", allocation),
             (70, "8\n7\n9\n", allocation),
             (0, "7\n8\n8\n9\n", ""),
             (70, "9\n", f"<std/memory>:{mutable[0]}: runtime error[GTI-R0008]: "
                           "empty_owner_access in dereference\n"),
             (70, "9\n", f"<std/memory>:{mutable[0]}: runtime error[GTI-R0008]: "
                           "empty_owner_access in dereference\n"),
             (70, "8\n9\n", allocation),
             (70, "9\n", allocation)]
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-owning-policy-") as temporary:
        for name, flags, native in modes:
            binary = Path(temporary) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(fixtures / "unique_scalar_allocation_fault.cpp"), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, phase, result.returncode, result.stdout, result.stderr, expected)
            print(f"owning-function policy failure: {name} passed", flush=True)


if __name__ == "__main__":
    main()
