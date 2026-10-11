#!/usr/bin/env python3
"""A member initializer constructs its field from an owning Expected call
result passed directly by value, including when that construction or a later
member fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/owned_argument_member_initializer.gti"
    lines = fixture.read_text().splitlines()
    access = next(i + 1 for i, text in enumerate(lines)
                  if "number(item.value().number)" in text)
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "102\n5\n102\n5\n5\n102\n7\n", "")),
        (("member-failure",),
         (70, "5\n", f"{fixture.name}:{access}: runtime error[GTI-R0009]: "
                     "invalid_expected_access in value_on_error\n")),
    ]})
