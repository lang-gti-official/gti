#!/usr/bin/env python3
"""A constructor's Expected field is initialized directly from a call whose
payload owns resources, including when the producer or a later member
fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/owning_expected_field.gti"
    lines = fixture.read_text().splitlines()
    site = lambda text: next(i + 1 for i, line in enumerate(lines) if text in line)
    producer = site("return counted(value + 1);")
    member = site("sum(value + 1)")
    overflow = "runtime error[GTI-R0001]: integer_overflow in addition\n"
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "4\n4\n", "")),
        (("producer-failure",),
         (70, "", f"{fixture.name}:{producer}: {overflow}")),
        (("member-failure", "rollback"),
         (70, "4\n", f"{fixture.name}:{member}: {overflow}")),
    ]})
