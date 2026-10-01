#!/usr/bin/env python3
"""Constructor field placement/replacement preserves success and failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    cases = {}
    for name, success, failure_output in (
        ("constructor_nonassignable_fields", "7\n12\n3\n2\n1\n", "7\n2\n1\n"),
        ("constructor_field_call_assignment", "12\nold\n12\nnew\ndone\nnew\n",
         "12\nold\nnew\n"),
    ):
        source = fixtures / f"{name}.gti"
        line = next(index + 1 for index, text in enumerate(source.read_text().splitlines())
                    if "12 / divisor" in text)
        failure = (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                   "division_by_zero in integer_division\n")
        cases[name] = [((), (0, success, "")), (("fail",), (70, failure_output, failure))]
    main(names=tuple(cases), cases=cases)
