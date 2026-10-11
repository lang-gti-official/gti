#!/usr/bin/env python3
"""Moving a cleanup-free expected<T, E> into a new binding constructs that
binding from its source, including when the success payload has no default
constructor and when a later call fails while the moved bindings are live."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_cleanup_free_move.gti"
    lines = fixture.read_text().splitlines()
    site = lambda text: next(i + 1 for i, line in enumerate(lines) if text in line)
    overflow_site = site("return value + 1;")
    overflow = "runtime error[GTI-R0001]: integer_overflow in addition\n"
    output = "7\n11\n4\n6\n7\n"
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, output, "")),
        (("fail",),
         (70, output, f"{fixture.name}:{overflow_site}: {overflow}")),
    ]})
