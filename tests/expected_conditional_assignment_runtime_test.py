#!/usr/bin/env python3
"""A conditional whose arms are Expected<Class, E> call results replaces an
existing binding, or a field of one, with an integral enum or a scalar error.
Both arms succeed or fail, nest, and repeat in a loop. A cleanup-free success
class without a default constructor publishes through the shared join; an
owning one retires each replaced value exactly once, also when an arm raises."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_conditional_assignment.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    failure = (f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "12\n-2\n-1\n62\n72\n-2\n92\n112\n-2\n132\n-5\n64\n74\n122\n"
                 "52\n62\n-2\n100\n101\n106\n108\n114\n120\n131\n122\n122\n", "")),
        (("cleanup-free-arm-failure",), (70, "", failure)),
        (("owning", "arm-failure"), (70, "9\n", failure)),
        (("owning", "nested", "arm-failure"), (70, "7\n", failure)),
    ]})
