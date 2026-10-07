#!/usr/bin/env python3
"""An inherited primary forbids callee-local cleanup after a second failure."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/inherited_cleanup_context.gti"
    lines = fixture.read_text().splitlines()

    def site(text):
        return next(i for i, line in enumerate(lines, 1) if text in line)

    secondary = site("secondary =")
    primary = site("int primary =")
    worker = site("worker_primary =")
    after_success = site("after_success =")

    def ordinary(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(line, code):
        return (f"{fixture.name}:{secondary}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [{code}] at "
                f"{fixture.name}:{line}; secondary [GTI-R0002]\n")

    main(names=("inherited_cleanup_context",), execution_profile="concurrent", cases={
        "inherited_cleanup_context": [
            ((), (0, "2\n3\n1\n", "")),
            (("phase",), (70, "2\n3\n1\n", ordinary(secondary))),
            (("phase",) * 2, (70, "2\n", emergency(primary, "GTI-R0002"))),
            (("phase",) * 3, (70, "2\n3\n1\n", ordinary(after_success))),
            (("phase",) * 4, (70, "2\n", emergency(worker, "GTI-R0003"))),
            (("phase",) * 5, (70, "2\n3\n1\n", ordinary(secondary))),
            (("phase",) * 6, (70, "2\n3\n1\n", ordinary(after_success))),
        ]
    })
