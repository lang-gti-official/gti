#!/usr/bin/env python3
"""A returned consuming-member result outlives its receiver's cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def returned_result_cases(name="consuming_returned_result"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    receiver = line("int guard = 1 / this.divisor")
    result = line("int fragile = 1 / this.divisor")
    primary = (f"{fixture.name}:{receiver}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    double = (f"{fixture.name}:{result}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{receiver}; secondary [GTI-R0002]\n")
    # Each returned result is destroyed once, by its caller. A receiver that
    # fails after the call leaves the unpublished result to failure cleanup.
    returned = "1\n-1\n2\n3\n-3\n-2\n4\n-4\n5\n-5\n6\n-6\n7\n8\n-7\n-8\n9\n"
    return {fixture.stem: [
        ((), (0, returned + "kept\n-9\n", "")),
        (("fail",), (70, returned + "-9\n", primary)),
        (("fail", "double"), (70, returned + "-9\n", double)),
    ]}


if __name__ == "__main__":
    main(names=("consuming_returned_result",), cases=returned_result_cases())
