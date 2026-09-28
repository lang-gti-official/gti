#!/usr/bin/env python3
"""Consuming erased callbacks retain captures through the caller expression."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    fixture = fixtures / "owning_function_consuming_lifetime.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def secondary(expression):
        primary = next(i + 1 for i, text in enumerate(lines) if expression in text)
        cleanup = next(i + 1 for i, text in enumerate(lines)
                       if "1 / this.cleanup_divisor" in text)
        return (f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")

    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
    empty_site = next(i + 1 for i, line in enumerate(memory.read_text().splitlines())
                      if "return gti_internal::unique_owner_borrow_mut(" in line)
    expected = [
        (0, "1\n2\n3\n7\n5\n99\n", ""),
        (70, "1\n7\n99\n", failure("value.read() / divisor")),
        (70, "1\n2\n7\n99\n", failure("6 / divisor")),
        (70, "99\n", f"<std/memory>:{empty_site}: runtime error[GTI-R0008]: "
                      "empty_owner_access in dereference\n"),
        (70, "1\n2\n3\n7\n99\n", failure("1 / this.cleanup_divisor")),
        (70, "1\n7\n", secondary("value.read() / divisor")),
        (70, "1\n2\n7\n", secondary("6 / divisor")),
    ]
    main(names=("consuming_polymorphic_receiver", fixture.stem,
                "owning_function_consuming", "owning_function_consuming_arguments"), cases={
        "consuming_polymorphic_receiver": [((), (0, "2\n4\n1\n9\n", ""))],
        "owning_function_consuming": [((), (0, "9\n", ""))],
        "owning_function_consuming_arguments": [((), (0, "", ""))],
        fixture.stem: [(tuple(["case"] * mode), result)
                       for mode, result in enumerate(expected)],
    })
