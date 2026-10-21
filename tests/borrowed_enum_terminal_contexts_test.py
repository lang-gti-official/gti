#!/usr/bin/env python3
"""Excluded reference-result domains preserve failure and cleanup in expressions."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    name = "borrowed_enum_terminal_contexts"
    fixture = pathlib.Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(message):
        line = next(index + 1 for index, text in enumerate(lines)
                    if f'panic("{message}")' in text)
        return (70, "99\n", f"{fixture.name}:{line}: runtime error[GTI-R0017]: "
                f'panic: "{message}"\n')

    main(names=(name,), cases={name: [
        ((), (0, "99\n", "")),
        (("binding",), failure("missing observation")),
        (("nested", "argument"), failure("missing observation")),
        (("consumed", "receiver", "result"), failure("missing consumed observation")),
        (("reference", "return", "path", "result"), failure("missing observation")),
    ]})
