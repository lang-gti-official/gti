#!/usr/bin/env python3
"""Contextual arithmetic before raw writes, checked failures after them."""

from pathlib import Path
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/optimizer_raw_context.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "return destination + 1" in text)
    overflow = (f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    cases = {
        "optimizer_raw_context": [
            ((), (0, "8\n2147483647\n", "")),
            (("overflow",), (70, "8\n2147483647\n", overflow)),
        ]
    }
    source_path = fixture.parent / "optimizer_scalar_addresses.gti"
    source = source_path.read_text()
    functions = ("bad_replaced", "bad_joined", "bad_alias", "bad_raw_write",
                 "bad_pointer_alias", "bad_lifetime")
    cases["scalar_addresses"] = [((), (0, "", ""))]
    paths = {"optimizer_raw_context": fixture, "scalar_addresses": source_path}
    with tempfile.TemporaryDirectory(prefix="gti-scalar-addresses-") as temporary:
        for function in functions:
            name = f"scalar_addresses_{function}"
            path = Path(temporary) / f"{name}.gti"
            call = f"{function}(true)" if function == "bad_joined" else f"{function}()"
            dispatch = f"if (false) return {call};"
            assert source.count(dispatch) == 1
            path.write_text(source.replace(dispatch, f"if (true) return {call};"))
            paths[name] = path
            lines = source.splitlines()
            start = next(index for index, text in enumerate(lines, 1)
                         if text.startswith(f"int {function}("))
            line = next(index for index, text in enumerate(lines, 1)
                        if index > start and "return *pointer + 1;" in text)
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")
            cases[name] = [((), (70, "", error))]
        main(names=tuple(paths), cases=cases, fixture_paths=paths)
