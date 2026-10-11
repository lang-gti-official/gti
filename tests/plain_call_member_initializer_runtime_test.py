#!/usr/bin/env python3
"""Constructor fields initialized directly from calls that cannot fail,
including when an argument or a later member fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/plain_call_member_initializer.gti"
    lines = fixture.read_text().splitlines()
    site = lambda text: next(i + 1 for i, line in enumerate(lines) if text in line)
    argument = site("owned(make_counted(number + 1))")
    member = site("sum(number + 1)")
    overflow = "runtime error[GTI-R0001]: integer_overflow in addition\n"
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "4\n5\n", "")),
        (("argument-failure",),
         (70, "", f"{fixture.name}:{argument}: {overflow}")),
        (("member-failure", "rollback"),
         (70, "4\n", f"{fixture.name}:{member}: {overflow}")),
    ]})
