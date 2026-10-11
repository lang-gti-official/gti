#!/usr/bin/env python3
"""Expected call results return across pending scope cleanup.

A transformed callee's Expected<Class, E> or Expected<Enum, E> result is
returned directly or bound, moved and returned while another local still owes
cleanup, including from inside a range loop and a lambda. The cleanup runs
before publication, and a failing cleanup after the result exists still
propagates its defined failure.
"""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_scoped_return.gti"
    lines = fixture.read_text().splitlines()
    guard_site = next(i + 1 for i, text in enumerate(lines)
                      if "int32_t checked = 2 / divisor;" in text)
    scoped = [
        ((), (0, "72\n-1\n33\n-1\n1\n-1\n0\n-1\n12\n-2\n0\n-2\n1\n-1\n", "")),
        (("guard",), (0, "2\n73\n2\n1\n", "")),
        (("guard", "fail"),
         (70, "2\n",
          f"{fixture.name}:{guard_site}: runtime error[GTI-R0002]: "
          "division_by_zero in integer_division\n")),
    ]
    main(names=("expected_scoped_return", "expected_scoped_return_class_direct",
                "expected_scoped_return_enum_bound",
                "expected_scoped_return_enum_loop"),
         cases={"expected_scoped_return": scoped})
