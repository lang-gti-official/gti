#!/usr/bin/env python3
"""Safe wait/notify under every native mode the backend policy selects."""
from pathlib import Path
import sys
import tempfile

from atomic_runtime_test import build, execute
from backend_modes import cpp_only_case, is_cpp_mode, native_modes


def main() -> None:
    compiler, fixture, example = map(lambda p: Path(p).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="gti-atomic-wait-") as directory:
        modes = native_modes()
        for label, flags, _native in modes:
            for source in (fixture, example):
                binary = Path(directory) / "waiting"
                build(compiler, source, binary, flags=flags)
                for _ in range(3):
                    result = execute([str(binary)], timeout=30)
                    expected = "2\n" if source == example else ""
                    assert result.stdout == expected and not result.stderr, (label, result)
        # Counts GTI's no-throw allocation seam, not all host/libc allocations.
        # The shared observer has a separate managed-task positive control.
        # The provider replaces the C++ text's nothrow operator new, which the
        # LLVM backend's runtime allocation entry never calls, so only the C++
        # profile can observe it.
        cpp_modes = [(label, flags) for label, flags, _native in modes if is_cpp_mode(flags)]
        if cpp_modes:
            label, flags = cpp_modes[-1]
            binary = Path(directory) / "allocations"
            execute([str(compiler), "--execution-profile", "concurrent", *flags,
                     str(fixture.with_name("atomic_wait_allocations.gti")),
                     "-o", str(binary), "--",
                     str(fixture.with_name("scoped_parallel_allocations.cpp"))])
            execute([str(binary)], timeout=30)
        else:
            cpp_only_case("transport-allocation oracle counts the C++ text's nothrow operator new",
                          "atomic_wait_allocations", "cpp-c++23-O3")
    print("atomic waiting matrix passed")


if __name__ == "__main__":
    main()
