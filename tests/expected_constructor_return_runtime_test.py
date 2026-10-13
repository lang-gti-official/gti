#!/usr/bin/env python3
"""Constructions consuming an Expected<Class, E> argument publish directly at
their Return or constructor field, without default-constructing the payload."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_constructor_return.gti"
    lines = fixture.read_text().splitlines()

    def line(text):
        return next(i + 1 for i, source in enumerate(lines) if text in source)

    access = (f"{fixture.name}:{line('number(item.value().number)')}: "
              "runtime error[GTI-R0009]: invalid_expected_access in "
              "value_on_error\n")
    overflow = (f"{fixture.name}:{line('return value + 1;')}: runtime "
                "error[GTI-R0001]: integer_overflow in addition\n")
    prepared = fixture.with_name("prepared_checked_payload.gti")
    prepared_lines = prepared.read_text().splitlines()

    def prepared_line(text):
        return next(i + 1 for i, source in enumerate(prepared_lines) if text in source)

    prepared_access = (
        f"{prepared.name}:{prepared_line('Box failing{parsed.value(), 1}')}: "
        "runtime error[GTI-R0009]: invalid_expected_access in value_on_error\n")
    prepared_overflow = (
        f"{prepared.name}:{prepared_line(': item(input), total(input.number + after)')}: "
        "runtime error[GTI-R0001]: integer_overflow in addition\n")
    main(names=(fixture.stem, prepared.stem), cases={fixture.stem: [
        ((), (0, "", "")),
        (("returned-construction-failure",), (70, "", access)),
        (("later-argument-failure", "returned"), (70, "", overflow)),
        (("member-construction-failure", "of", "owner"), (70, "", access)),
    ], prepared.stem: [
        ((), (0, "", "")),
        (("invalid",), (70, "cleanup\n", prepared_access)),
        (("constructor", "failure"), (70, "cleanup\n", prepared_overflow)),
    ]})
