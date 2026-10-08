#!/usr/bin/env python3
"""Contextual arithmetic before raw writes, checked failures after them."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/optimizer_raw_context.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "return destination + 1" in text)
    overflow = (f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    main(names=("optimizer_raw_context",), cases={
        "optimizer_raw_context": [
            ((), (0, "8\n2147483647\n", "")),
            (("overflow",), (70, "8\n2147483647\n", overflow)),
        ]
    })
