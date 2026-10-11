#!/usr/bin/env python3
"""Expected<Class, E> call temporaries are observed in place through their
state test or one checked value()/error(), without default-constructing the
payload."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_temporary_observation.gti"
    lines = fixture.read_text().splitlines()

    def line(text):
        return next(i + 1 for i, source in enumerate(lines) if text in source)

    def failure(text, message):
        return f"{fixture.name}:{line(text)}: runtime error[{message}\n"

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("value-on-error",),
         (70, "", failure("return make_item(no).value().number;",
                          "GTI-R0009]: invalid_expected_access in "
                          "value_on_error"))),
        (("error-on-value", "temporary"),
         (70, "", failure("return make_item(yes).error();",
                          "GTI-R0009]: invalid_expected_access in "
                          "error_on_value"))),
        (("producer-failure", "before", "observation"),
         (70, "", failure("return item_value(value + 1);",
                          "GTI-R0001]: integer_overflow in addition"))),
    ]})
