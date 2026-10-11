#!/usr/bin/env python3
"""Class-valued conditionals bound, returned, passed, read, assigned, nested
and used as member initializers, including when an arm or a later member
fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/class_conditional_join.gti"
    lines = fixture.read_text().splitlines()
    site = lambda text: next(i + 1 for i, line in enumerate(lines) if text in line)
    arm = site("return plain_value(number + 1);")
    member = site("sum(number + 1)")
    overflow = "runtime error[GTI-R0001]: integer_overflow in addition\n"
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "6\n", "")),
        (("arm-failure",), (70, "", f"{fixture.name}:{arm}: {overflow}")),
        (("member-failure", "rollback"),
         (70, "3\n", f"{fixture.name}:{member}: {overflow}")),
    ]})
