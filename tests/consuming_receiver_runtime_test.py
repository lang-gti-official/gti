#!/usr/bin/env python3
"""Consuming receivers retain caller-owned full-expression cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def lifetime_cases(name="consuming_receiver_lifetime"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    return {fixture.stem: [
        ((), (0, "2\n3\n4\n1\n5\n9\n", "")),
        (("body",), (70, "2\n1\n9\n", failure("12 / divisor"))),
        (("later", "argument"),
         (70, "2\n3\n1\n9\n", failure("6 / divisor"))),
    ]}


def failure_cases(name="consuming_receiver_failure"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(expression):
        return next(i + 1 for i, text in enumerate(lines) if expression in text)

    cleanup = line("1 / this.cleanup_divisor")
    body = line("12 / divisor")
    argument = line("int checked = 1 / divisor")
    later = line("return 1 / divisor")

    def report(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def secondary(primary):
        return (f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")

    expected = [
        (0, "6\n21\n3\n22\n4\n2\n1\n5\n9\n", ""),
        (70, "6\n21\n1\n2\n9\n", report(body)),
        (70, "6\n21\n3\n2\n1\n9\n", report(later)),
        (70, "6\n1\n2\n9\n", report(argument)),
        (70, "6\n21\n3\n22\n4\n2\n1\n9\n", report(cleanup)),
        (70, "6\n21\n1\n", secondary(body)),
        (70, "6\n21\n3\n2\n", secondary(later)),
        (70, "6\n21\n3\n22\n4\n2\n1\n", secondary(cleanup)),
    ]
    return {fixture.stem: [(tuple(["case"] * i), result)
                           for i, result in enumerate(expected)]}


def field_cases():
    fixture = pathlib.Path(__file__).parent / "fixtures/consuming_receiver_fields.gti"
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    diagnostic = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                  "division_by_zero in integer_division\n")
    return {fixture.stem: [
        ((), (0, "1\n3\n4\n2\n5\n9\n", "")),
        (("body",), (70, "1\n2\n9\n", diagnostic)),
    ]}


def generic_cases(name="consuming_lambda_generic"):
    cases = lifetime_cases(name)[name]
    # The generic callee owns the moved parameter: its receiver's full
    # expression ends before the caller evaluates the later argument.
    return {name: [
        (cases[0][0], (0, "2\n1\n3\n4\n5\n9\n", "")),
        cases[1],
        (cases[2][0], (70, "2\n1\n3\n9\n", cases[2][1][2])),
    ]}


if __name__ == "__main__":
    main(names=("consuming_receiver_lifetime", "consuming_receiver_failure",
                "consuming_receiver_fields", "consuming_virtual_receiver",
                "consuming_generated_field_default"),
         cases=lifetime_cases() | failure_cases() | field_cases() |
               lifetime_cases("consuming_virtual_receiver"))
