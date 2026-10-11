#!/usr/bin/env python3
"""The arms of an Expected<Class, E>-valued conditional may be callable value
invocations (closure literals, a capturing closure, callable parameters) or
copies and moves of named Expected bindings, whether the conditional
initializes a new binding or replaces an existing one. Arms succeed or fail
through enum and scalar errors, nest, and repeat in a loop. An owning success
class retires each replaced value and each live moved-from binding exactly
once, also when an arm raises."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_conditional_arms.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    failure = (f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "13\n-2\n-1\n63\n54\n-1\n52\n-2\n-2\n52\n112\n-1\n142\n52\n"
                 "183\n203\n52\n312\n54\n223\n52\n332\n74\n-7\n34\n-9\n"
                 "33053\n42998\n-936\n71998\n32\n-2\n"
                 "200\n202\n204\n208\n220\n230\n221\n222\n210\n207\n232\n"
                 "232210\n", "")),
        (("cleanup-free-arm-failure",), (70, "", failure)),
        (("owning", "arm-failure"), (70, "8\n9\n", failure)),
    ]})
