#!/usr/bin/env python3
"""Safe wait/notify under every native mode the backend policy selects."""
from pathlib import Path
import sys
import tempfile

from atomic_runtime_test import build, execute
from backend_modes import is_cpp_mode, native_modes


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
        # The provider replaces nothrow operator new, which the C++ text and
        # the runtime's owner allocation entry (LLVM) both call, so the last
        # mode of each selected backend proves that waiting allocates nothing.
        last_modes = {is_cpp_mode(flags): flags for _label, flags, _native in modes}
        for flags in last_modes.values():
            binary = Path(directory) / "allocations"
            execute([str(compiler), "--execution-profile", "concurrent", *flags,
                     str(fixture.with_name("atomic_wait_allocations.gti")),
                     "-o", str(binary), "--",
                     str(fixture.with_name("scoped_parallel_allocations.cpp"))])
            execute([str(binary)], timeout=30)
    print("atomic waiting matrix passed")


if __name__ == "__main__":
    main()
