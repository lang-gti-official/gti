#!/usr/bin/env python3
"""Public erased callbacks publish owned results and clean failures exactly once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    directory = pathlib.Path(__file__).parent / "fixtures"
    names = ("owning_function_resource_result", "owning_function_unique_result",
             "owning_function_string_result", "owning_function_expected_result")

    def failure(name, expression):
        source = directory / f"{name}.gti"
        line = next(index + 1 for index, text in enumerate(source.read_text().splitlines())
                    if expression in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    cases = {}
    for name in names:
        if name.endswith("expected_result"):
            success = "6\n" * 6 + "9\n"
            producer_failure = "9\n"
        elif name.endswith("string_result"):
            success = "90\nowned result\n90\n9\n"
            producer_failure = "90\n90\n9\n"
        else:
            success = "6\n6\n9\n"
            producer_failure = "9\n"
        cases[name] = [
            ((), (0, success, "")),
            (("producer",), (70, producer_failure, failure(name, "12 / divisor"))),
            (("later", "failure"), (70, success, failure(name, "6 / (argc"))),
        ]
    main(names=names, cases=cases)
