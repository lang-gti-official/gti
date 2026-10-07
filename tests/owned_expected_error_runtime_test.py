#!/usr/bin/env python3
"""Owning Expected errors retain exactly-once payload cleanup on every path."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/owned_expected_error.gti"
    lines = fixture.read_text().splitlines()
    construction = next(i + 1 for i, text in enumerate(lines) if "int checked = 1 / divisor" in text)
    later = next(i + 1 for i, text in enumerate(lines) if "int checked = 12 /" in text)
    wrong_scalar = next(i + 1 for i, text in enumerate(lines) if "int invalid = value.value()" in text)
    wrong_void = next(i + 1 for i, text in enumerate(lines) if text.strip() == "value.value();")
    wrong_class = next(i + 1 for i, text in enumerate(lines) if "return std::move(result).value();" in text)
    prefix = "1\n2\n3\n2\n5\n6\n4\n"
    output = prefix + "9\n"

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def wrong_state(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0009]: "
                "invalid_expected_access in value_on_error\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, output, "")),
        (("payload-fails",), (70, output, failure(construction))),
        (("later", "fails"), (70, output, failure(later))),
        (("wrong", "scalar", "state"), (70, prefix + "2\n9\n", wrong_state(wrong_scalar))),
        (("wrong", "void", "value", "state"), (70, prefix + "6\n9\n", wrong_state(wrong_void))),
        (("wrong", "class", "value", "return", "state"), (70, prefix + "5\n9\n", wrong_state(wrong_class))),
    ]})
