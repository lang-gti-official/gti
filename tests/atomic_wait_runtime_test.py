#!/usr/bin/env python3
"""Safe wait/notify across native standards and optimization levels."""
from pathlib import Path
import sys
import tempfile

from atomic_runtime_test import build, execute


def main() -> None:
    compiler, fixture, example = map(lambda p: Path(p).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="gti-atomic-wait-") as directory:
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O1", "-O3"):
                for source in (fixture, example):
                    binary = Path(directory) / "waiting"
                    build(compiler, source, binary, standard=standard,
                          optimization=optimization)
                    for _ in range(3):
                        result = execute([str(binary)], timeout=30)
                        expected = "2\n" if source == example else ""
                        assert result.stdout == expected and not result.stderr, result
    print("atomic waiting matrix passed")


if __name__ == "__main__":
    main()
