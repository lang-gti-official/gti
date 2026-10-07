#!/usr/bin/env python3
"""Public source-defined owning callback wrappers use the shared native matrix."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def mutable_argument_failure_cases():
    fixture = pathlib.Path(__file__).parent / "fixtures/owning_function_mutable_arguments_failure.gti"
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "value / divisor" in text)
    diagnostic = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                  "division_by_zero in integer_division\n")
    return {fixture.stem: [((), (0, "7\n1\n9\n", "")),
                           (("fail",), (70, "7\n1\n9\n", diagnostic))]}

if __name__ == "__main__":
    main(names=("owning_function", "owning_function_borrows", "owning_function_items",
                "owning_function_schema", "function_item_construction",
                "mutable_signature_callback", "qualified_signature_metadata",
                "signature_receiver_family", "owning_function_mutable",
                "owning_function_mutable_arguments",
                "owning_function_mutable_arguments_failure",
                "owning_function_temporary_target"),
         cases=mutable_argument_failure_cases())
