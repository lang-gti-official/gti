#!/usr/bin/env python3
"""Consuming instance methods and operator chains preserve owned lifetimes."""

from consuming_receiver_runtime_test import failure_cases, lifetime_cases
from lambda_pack_invocation_runtime_test import main
from pathlib import Path


def return_composition_cases():
    source = Path(__file__).parent / "fixtures/consuming_return_composition.gti"
    line = next(index + 1 for index, text in enumerate(source.read_text().splitlines())
                if "1 / this.divisor" in text)
    primary = (f"{source.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    double = (f"{source.name}:{line}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{source.name}:{line}; secondary [GTI-R0002]\n")
    return {"consuming_return_composition": [
        ((), (0, "2\n9\n7\n", "")),
        (("fail",), (70, "2\n7\n9\n", primary)),
        (("fail", "double"), (70, "2\n7\n", double)),
    ]}


def expected_receiver_cases():
    source = Path(__file__).parent / "fixtures/consuming_expected_receiver.gti"
    lines = source.read_text().splitlines()
    def failure(fragment, outcome):
        line = next(index + 1 for index, text in enumerate(lines) if fragment in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0009]: "
                f"invalid_expected_access in {outcome}\n")
    return {"consuming_expected_receiver": [
        ((), (0, "value\nerror\n9\n", "")),
        (("fail",), (70, "9\n", failure("value().take()", "value_on_error"))),
        (("fail", "error"), (70, "value\n9\n", failure("error().take()", "error_on_value"))),
    ]}


if __name__ == "__main__":
    main(names=("consuming_method_lifetime", "consuming_fresh_method_lifetime",
                "consuming_method_chains", "consuming_owned_builder",
                "consuming_method_failure", "consuming_value_operators",
                "consuming_conditional_lifetime", "consuming_return_composition",
                "consuming_receiver_producers", "consuming_expected_receiver",
                "consuming_wrapper_temporary"),
         cases=lifetime_cases("consuming_method_lifetime") |
               lifetime_cases("consuming_fresh_method_lifetime") |
               failure_cases("consuming_method_failure") |
               lifetime_cases("consuming_conditional_lifetime") |
               return_composition_cases() |
               expected_receiver_cases() |
               {"consuming_owned_builder": [((), (0, "1\n2\n2\na\nabcd\nabcd\n9\n9\n", ""))],
                "consuming_receiver_producers": [((), (0, "lambda\nlambda\nliteral\n7\nmoved\n", ""))]})
