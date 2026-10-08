#!/usr/bin/env python3
"""Ordinary constructor/result/receiver facts preserve checked execution."""

import pathlib
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/optimizer_object_context.gti"
    source = fixture.read_text()
    marker = "return danger.read();"
    if source.count(marker) != 1:
        raise AssertionError("the object's final runtime observation must be unique")
    lines = source.splitlines()
    hazard_line = next(index for index, line in enumerate(lines, 1)
                       if line == "class Hazard {")
    read_line = next(index for index, line in enumerate(lines, 1)
                     if "int read() { return this.value + 1; }" in line and index > hazard_line)
    changed_line = next(index for index, line in enumerate(lines, 1)
                        if "return this.value + 1;" in line and index > read_line)
    raw_line = next(index for index, line in enumerate(lines, 1)
                    if "return this.value + 1;" in line and index > changed_line)
    alias_start = next(index for index, line in enumerate(lines, 1)
                       if line == "class Aliased {")
    alias_line = next(index for index, line in enumerate(lines, 1)
                      if line.strip() == "return this.value + 1;" and index > alias_start)
    raw_constructor_start = next(index for index, line in enumerate(lines, 1)
                                 if line == "class RawConstructed {")
    raw_constructor_line = next(index for index, line in enumerate(lines, 1)
                                if "int read() { return this.value + 1; }" in line
                                and index > raw_constructor_start)
    variants = {
        "object_success": ("return 0;", None),
        "object_overflow": (marker, read_line),
        "object_changed": ("return danger.changed();", changed_line),
        "object_raw": ("return danger.raw_changed();", raw_line),
        "object_alias": ("return alias_case();", alias_line),
        "object_raw_constructor": ("return raw_constructor_case();", raw_constructor_line),
    }
    with tempfile.TemporaryDirectory(prefix="gti-object-context-source-") as temporary:
        paths = {}
        cases = {}
        for name, (replacement, line) in variants.items():
            path = pathlib.Path(temporary) / f"{name}.gti"
            path.write_text(source.replace(marker, replacement))
            paths[name] = path
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n") if line else ""
            cases[name] = [((), (70 if line else 0, "", error))]
        chain = []
        # Direct-mode discovery currently caps depth at 64. Keep this native
        # client within that independent frontend boundary; the owning C++
        # unit exercises the optimizer itself with 128 ordinary functions.
        chain_length = 48
        for index in reversed(range(chain_length)):
            value = "value" if index == chain_length - 1 else f"step{index + 1}(value)"
            chain.append(f"int step{index}(int value) {{ return {value} + 1; }}")
        chain.append(f"int main() {{ return step0(7) == {7 + chain_length} ? 0 : 1; }}")
        path = pathlib.Path(temporary) / "object_chain.gti"
        path.write_text("\n".join(chain))
        paths["object_chain"] = path
        cases["object_chain"] = [((), (0, "", ""))]
        main(names=tuple(paths), cases=cases, fixture_paths=paths)
