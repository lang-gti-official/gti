#!/usr/bin/env python3
"""Empty invocation and allocation failure obey ordinary owning-wrapper policy.

Small repeatable and once-invoked targets live inline, so cloning or replacing them succeeds
even while every allocation fails.
"""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run
from allocation_abort_runtime_test import ABORT_STATUS
from backend_modes import cpp_only_case, is_cpp_mode, native_modes


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "owning_function_policy_failure.gti"
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    lines = memory.read_text().splitlines()
    read = [i for i, line in enumerate(lines, 1)
            if "return gti_internal::unique_owner_borrow(" in line]
    mutable = [i for i, line in enumerate(lines, 1)
               if "return gti_internal::unique_owner_borrow_mut(" in line]
    assert len(read) == len(mutable) == 2
    allocation_site = next(i for i, line in enumerate(lines, 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    allocation = (f"<std/detail/memory_core>:{allocation_site}: runtime error[GTI-R0011]: "
                  "allocation_failure in unique_owner\n")
    # An empty repeatable wrapper reaches its boxed owner through get().
    empty = lambda site: (f"<std/detail/memory_core>:{site}: runtime error[GTI-R0008]: "
                          "empty_owner_access in dereference\n")
    cases = [(70, "9\n", empty(read[0])),
             (70, "9\n", empty(mutable[0])),
             (70, "9\n", empty(read[0])),
             (70, "9\n", empty(mutable[0])),
             (ABORT_STATUS, "", allocation),
             (ABORT_STATUS, "", allocation),
             (ABORT_STATUS, "", allocation),
             (0, "7\n8\n8\n9\n", ""),
             (70, "9\n", f"<std/detail/memory_core>:{mutable[0]}: runtime error[GTI-R0008]: "
                           "empty_owner_access in dereference\n"),
             (70, "9\n", f"<std/detail/memory_core>:{mutable[0]}: runtime error[GTI-R0008]: "
                           "empty_owner_access in dereference\n"),
             (ABORT_STATUS, "", allocation),
             (ABORT_STATUS, "", allocation),
             (42, "9\n", ""),
             (42, "9\n", ""),
             (42, "9\n", "")]
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-owning-policy-") as temporary:
        for name, flags, native in modes:
            binary = Path(temporary) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(fixtures / "unique_scalar_allocation_fault.cpp"), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            phases = list(enumerate(cases))
            if not is_cpp_mode(flags):
                # The fault provider (tests/fixtures/unique_scalar_allocation_fault.cpp)
                # replaces the C++ text's nothrow operator new; LLVM-backed
                # programs allocate through the runtime.
                cpp_only_case("allocation fault injection through the C++ text's nothrow operator new",
                              "allocation failure phases", name)
                phases = [(phase, expected) for phase, expected in phases if expected[0] != ABORT_STATUS]
            for phase, expected in phases:
                result = run([str(binary), *(["phase"] * phase)])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, phase, result.returncode, result.stdout, result.stderr, expected)
            print(f"owning-function policy failure: {name} passed", flush=True)


if __name__ == "__main__":
    main()
