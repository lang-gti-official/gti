#!/usr/bin/env python3
"""A field of an owning class temporary, whether a call result, a construction
or the payload of an owning Expected temporary, is read in place before the
temporary is destroyed exactly once."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/owning_temporary_field_read.gti"
    lines = fixture.read_text().splitlines()

    def failure(text, message):
        line = next(i + 1 for i, source in enumerate(lines) if text in source)
        return f"{fixture.name}:{line}: runtime error[{message}\n"

    overflow = "GTI-R0001]: integer_overflow in addition"
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("value-on-error",),
         (70, "", failure("return make_named(no).value().size;",
                          "GTI-R0009]: invalid_expected_access in "
                          "value_on_error"))),
        (("later-failure", "after-read"),
         (70, "", failure("return value + 1;", overflow))),
        (("producer-failure", "before", "read"),
         (70, "", failure("return counted(number + 1);", overflow))),
        # The failure cleanup destroys the constructed temporary once, after
        # its read; a failed construction is never destroyed.
        (("later-failure", "after", "constructed", "read"),
         (70, "destroyed 4\n", failure("return value + 1;", overflow))),
        (("constructor-failure", "before", "constructed", "read", "now"),
         (70, "", failure("checked_traced(int32_t number) : number(number + 1)",
                          overflow))),
    ]})
