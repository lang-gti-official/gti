#!/usr/bin/env python3
"""Fresh class prefixes before packs retain exact failure and staged cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/pack_prefix_constructor_failure.gti"
    line = next(index + 1 for index, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "10\n7\n2\n9\n", "")),
        (("first",), (70, "10\n2\n9\n", failure)),
        (("prior", "success"), (0, "10\n8\n2\n1\n9\n", "")),
        (("prior", "constructor", "failure"), (70, "10\n1\n2\n9\n", failure)),
    ]})
