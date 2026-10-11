#!/usr/bin/env python3
"""Expected<Class, E> call results initialize constructor fields and meet in
conditional joins without default-constructing the payload."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_field_and_join.gti"
    lines = fixture.read_text().splitlines()

    def failure(text, message):
        line = next(i + 1 for i, source in enumerate(lines) if text in source)
        return f"{fixture.name}:{line}: runtime error[{message}\n"

    overflow = failure("return item_value(value + 1);",
                       "GTI-R0001]: integer_overflow in addition")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("joined-value-on-error",),
         (70, "", failure("(no ? make_item(true) : make_item(false)).value()",
                          "GTI-R0009]: invalid_expected_access in "
                          "value_on_error"))),
        (("joined-producer-failure", "arm"), (70, "", overflow)),
        (("field-producer-failure", "of", "member"), (70, "", overflow)),
    ]})
