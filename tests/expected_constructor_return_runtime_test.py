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
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("returned-construction-failure",), (70, "", access)),
        (("later-argument-failure", "returned"), (70, "", overflow)),
        (("member-construction-failure", "of", "owner"), (70, "", access)),
    ]})
