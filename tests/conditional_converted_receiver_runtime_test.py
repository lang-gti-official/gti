#!/usr/bin/env python3
"""A field-path receiver stays staged across conditional checked-conversion arguments."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def receiver_cases():
    fixture = (pathlib.Path(__file__).parent
               / "fixtures/conditional_converted_receiver_field.gti")
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "// conversion-failure" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0006]: "
               "numeric_conversion_out_of_range in numeric_cast\n")
    return {fixture.stem: [
        ((), (0, "2\n1\n0\n10\n2\n", "")),
        (("fail",), (70, "2\n1\n0\n", failure)),
    ]}


if __name__ == "__main__":
    main(names=("conditional_converted_receiver_field",),
         cases=receiver_cases())
