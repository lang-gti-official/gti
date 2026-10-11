#!/usr/bin/env python3
"""Expected<Class, E> bindings whose error alternative is an integral enum are
replaced by source function and callable invocation results, copies, moves,
Unexpected values and payloads. A cleanup-free success class without a default
constructor publishes into its own storage; an owning one retires each
replaced value exactly once, also when the replacing call raises."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_call_reassignment.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    failure = (f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "32\n72\n-1\n-2\n42\n53\n-2\n82\n-1\n54\n-1\n67\n32\n-1\n"
                 "64\n92\n-2\n62\n-2\n11\n7\n1\n2\n5\n5\n6\n7\n8\n8\n", "")),
        (("replacing-call-failure",), (70, "", failure)),
        (("owning", "replacing-call-failure"), (70, "9\n", failure)),
    ]})
