#!/usr/bin/env python3
"""Borrowed receivers survive a conditional fallible argument after an owning one."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def receiver_cases():
    fixture = (pathlib.Path(__file__).parent
               / "fixtures/conditional_owning_argument_receivers.gti")
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    cases = {fixture.stem: [
        ((), (0, "5\n17\n1\n13\n6\n118\n2\n114\n", "")),
        (("fresh-lambda",), (70, "5\n2\n1\n", failure)),
        (("moved", "lambda"), (70, "5\n17\n1\n2\n", failure)),
        (("fresh", "method", "call"), (70, "5\n17\n1\n13\n6\n2\n", failure)),
        (("moved", "method", "call", "argument"),
         (70, "5\n17\n1\n13\n6\n118\n2\n", failure)),
    ]}
    expected = fixture.with_name("conditional_expected_argument_receiver.gti")
    lines = expected.read_text().splitlines()
    division_line = next(i + 1 for i, text in enumerate(lines)
                         if "12 / divisor" in text)
    extraction_line = next(i + 1 for i, text in enumerate(lines)
                           if "receiver.owner(std::move" in text)
    division = (f"{expected.name}:{division_line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    wrong_state = (f"{expected.name}:{extraction_line}: runtime error[GTI-R0009]: "
                   "invalid_expected_access in value_on_error\n")
    cases[expected.stem] = [
        ((), (0, "owner accepted\n10\nobject accepted\n20\n18\n", "")),
        (("owner-failure",), (70, "10\n", division)),
        (("missing", "value"), (70, "", wrong_state)),
        (("later", "class", "failure"),
         (70, "owner accepted\n10\n20\n", division)),
    ]
    ordinary = fixture.with_name("consuming_owned_conditional_argument.gti")
    division_line = next(i + 1 for i, text in enumerate(ordinary.read_text().splitlines())
                         if "12 / value" in text)
    division = (f"{ordinary.name}:{division_line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    cases[ordinary.stem] = [
        ((), (0, "accepted\n7\n12\n", "")),
        (("failure",), (70, "7\n", division)),
    ]
    array = fixture.with_name("passive_result_array_assignment.gti")
    array_lines = array.read_text().splitlines()
    division_line = next(i + 1 for i, text in enumerate(array_lines) if "12 / divisor" in text)
    assignment_line = next(i + 1 for i, text in enumerate(array_lines) if "packets[argc] =" in text)
    cases[array.stem] = [
        ((), (0, "12\n9\n", "")),
        (("bounds",), (70, "9\n", f"{array.name}:{assignment_line}: runtime error[GTI-R0007]: "
                       "index_out_of_bounds in fixed_array\n")),
        (("producer", "failure"), (70, "9\n", f"{array.name}:{division_line}: runtime error[GTI-R0002]: "
                                   "division_by_zero in integer_division\n")),
    ]
    payload = fixture.with_name("passive_result_payload.gti")
    payload_line = next(i + 1 for i, text in enumerate(payload.read_text().splitlines())
                        if "12 / divisor" in text)
    cases[payload.stem] = [
        ((), (0, "7\n12\n9\n", "")),
        (("failure",), (70, "7\n9\n", f"{payload.name}:{payload_line}: runtime error[GTI-R0002]: "
                         "division_by_zero in integer_division\n")),
    ]
    return cases


if __name__ == "__main__":
    main(names=("conditional_owning_argument_receivers",
                "conditional_expected_argument_receiver",
                "consuming_owned_conditional_argument",
                "passive_result_array_assignment", "passive_result_payload"),
         cases=receiver_cases())
