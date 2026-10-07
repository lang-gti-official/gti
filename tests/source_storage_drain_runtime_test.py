#!/usr/bin/env python3
"""Source cleanup guards drain remaining storage without recovering failure."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/source_storage_drain.gti"
    lines = fixture.read_text().splitlines()
    destructor = next(index for index, text in enumerate(lines, 1)
                      if "1 / this.divisor" in text)
    body = next(index for index, text in enumerate(lines, 1)
                if "1 / divisor" in text)

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    double = (f"{fixture.name}:{destructor}: runtime error[GTI-R0014]: "
              "failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{destructor}; secondary [GTI-R0002]\n")
    main(names=("source_storage_drain",), cases={"source_storage_drain": [
        ((), (0, "3\n103\n2\n102\n1\n101\n800\n", "")),
        (("fail",), (70, "3\n103\n800\n2\n102\n1\n101\n", failure(destructor))),
        # The first failure drains its member (103). The second is an
        # emergency: stop before that destructor's member (102) or any
        # remaining slot cleanup, preserving both exact failure records.
        (("fail", "twice"), (70, "3\n103\n800\n2\n", double)),
        (("body", "failure", "only"),
         (70, "800\n3\n103\n2\n102\n1\n101\n", failure(body))),
    ]})
