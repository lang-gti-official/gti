#!/usr/bin/env python3
"""Moved closure environments use MIR full-expression cleanup, including failure."""

import pathlib

from consuming_receiver_runtime_test import failure_cases, generic_cases, lifetime_cases
from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/contextual_callable_arguments.gti"
    failure_line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                        if "6 / value" in text)
    failure = (f"{fixture.name}:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    contextual_cases = {fixture.stem: [
        ((), (0, "3\n2\n9\n", "")),
        (("first",), (70, "0\n9\n", failure)),
        (("second", "argument"), (70, "3\n0\n9\n", failure)),
    ]}
    main(names=("consuming_lambda_lifetime", "consuming_lambda_failure",
                "consuming_lambda_generic", "consuming_lambda_pack",
                "consuming_lambda_nested", "contextual_callable_arguments"),
         cases=lifetime_cases("consuming_lambda_lifetime") |
               failure_cases("consuming_lambda_failure") | generic_cases() |
               generic_cases("consuming_lambda_pack") |
               lifetime_cases("consuming_lambda_nested") | contextual_cases)
