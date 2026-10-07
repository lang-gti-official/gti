#!/usr/bin/env python3
"""Nested nominal results publish only on success, including erased callbacks."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/nested_nominal_result.gti"
    owned = fixture.with_name("nested_nominal_owned_callback.gti")

    def failure(expression, source=fixture):
        line = next(i + 1 for i, text in enumerate(source.read_text().splitlines())
                    if expression in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem, owned.stem, "callable_adapter_declaration_order",
                "passive_constructor_field"), cases={fixture.stem: [
        ((), (0, "6\n106\n8\n3\n103\n7\n2\n202\n8\n9\n", "")),
        (("member",), (70, "9\n", failure("12 / divisor"))),
        (("later", "argument"), (70, "6\n9\n", failure("24 /"))),
        (("erased", "callback", "failure"),
         (70, "6\n106\n8\n9\n", failure("12 / divisor"))),
        (("constructor", "argument", "producer", "failure"),
         (70, "6\n106\n8\n3\n103\n7\n9\n", failure("12 / divisor"))),
    ], owned.stem: [
        ((), (0, "6\n106\n8\n8\n90\n9\n", "")),
        (("callback",), (70, "90\n9\n", failure("12 / divisor", owned))),
        (("later", "argument"), (70, "6\n90\n9\n", failure("24 /", owned))),
    ]})
