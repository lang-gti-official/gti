#!/usr/bin/env python3
"""Expected<Class, E> bindings are replaced as a whole by call results,
copies, moves, Unexpected values and payload constructions. Each value an
owning call result replaces is retired exactly once."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_class_assignment.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    failure = (f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n2\n5\n", "")),
        (("replacing-call-failure",), (70, "", failure)),
        (("owning", "replacing-call-failure"), (70, "6\n", failure)),
    ]})
