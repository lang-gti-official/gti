#!/usr/bin/env python3
"""Constructor field placement/replacement preserves success and failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    cases = {}
    for name, success, failure_output in (
        ("constructor_cleanup_free_failure", "12\n2\n1\n", "1\n"),
        ("expected_constructor_payload", "12\n2\n1\n9\n", "1\n9\n"),
        ("optional_constructor_failure", "3\n12\n3\n9\n", "3\n3\n9\n"),
        ("optional_reset_failure", "3\ndone\n9\n", "3\n9\n"),
        ("constructor_nonassignable_fields", "7\n12\n3\n2\n1\n", "7\n2\n1\n"),
        ("constructor_field_call_assignment", "12\nold\n12\nnew\ndone\nnew\n",
         "12\nold\nnew\n"),
        ("constructor_nondefault_cloned_field", "12\nsource\nsource\nholder\nguard\n",
         "guard\n"),
    ):
        source = fixtures / f"{name}.gti"
        line = next(index + 1 for index, text in enumerate(source.read_text().splitlines())
                    if "12 / divisor" in text)
        failure = (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                   "division_by_zero in integer_division\n")
        cases[name] = [((), (0, success, "")), (("fail",), (70, failure_output, failure))]
    source = fixtures / "constructor_scalar_side_effects.gti"
    lines = source.read_text().splitlines()
    increment_line = next(index + 1 for index, text in enumerate(lines)
                          if "prefix(++counter)" in text)
    compound_line = next(index + 1 for index, text in enumerate(lines)
                         if "compound(counter += 2)" in text)
    partial = "100\n2147483647\n900\n2147483647\n"
    def overflow(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    cases[source.stem] = [
        ((), (0, "11\n11\n14\n40\n40\n300\n200\n40\n100\n40\n900\n40\n", "")),
        (("prefix",), (70, partial, overflow(increment_line))),
        (("postfix", "overflow"), (70, partial, overflow(increment_line))),
        (("compound", "overflow", "here"), (70, partial, overflow(compound_line))),
    ]
    main(names=tuple(cases), cases=cases)
