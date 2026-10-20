#!/usr/bin/env python3
"""Lambda invocations returning a class without a default constructor publish
into a source call's result storage for each consumer, including when a later
argument or the invocation raises, and an uninvoked lambda spells no body."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def lambda_class_result_cases(name="lambda_class_result_consumers"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    # A destroyed Res prints its negated id: a field-read result at the end
    # of its full expression, a moved argument inside its callee, a discarded
    # result at once, an assigned binding's old value at the assignment, a
    # short-circuit operand's result only when the operand is evaluated, and
    # the constructor field and the binding at scope exit.
    shapes = trace(1, 102, 7, 1, 6, 7, 8, 20, 11,
                   12, -12, -13, 13, -14, 15, -16, 17, 1, -18, 1,
                   7, -17, -15)
    return {fixture.stem: [
        ((), (0, shapes, "")),
        # The staged result is destroyed once by the later argument's failure.
        (("later-argument-failure",),
         (70, trace(-6),
          f"{fixture.name}:{line('return value + 1;')}: runtime "
          "error[GTI-R0001]: integer_overflow in addition\n")),
        # A raising invocation publishes no result, so nothing is destroyed.
        (("invocation-failure", "divide"),
         (70, "",
          f"{fixture.name}:{line('return Plain(84 / v);')}: runtime "
          "error[GTI-R0002]: division_by_zero in integer_division\n")),
    ]}


if __name__ == "__main__":
    main(names=("lambda_class_result_consumers",),
         cases=lambda_class_result_cases())
