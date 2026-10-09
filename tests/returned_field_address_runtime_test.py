#!/usr/bin/env python3
"""Returned field addresses preserve values, never stale pointee proofs."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    fixture = fixtures / "optimizer_returned_field_address.gti"
    original = fixture.read_text()
    native = fixtures / "optimizer_returned_field_address_native.cpp"
    negatives = {
        "bad_different_field": "owner.bad_different_field()",
        "bad_ambiguous_field": "owner.bad_ambiguous_field(false)",
        "bad_mutating_field": "owner.bad_mutating_field()",
        "bad_opaque_field": "owner.bad_opaque_field()",
        "bad_reassigned_nested": "relay.bad_reassigned_nested()",
        "bad_opaque_nested": "relay.bad_opaque_nested()",
    }
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-returned-field-") as directory:
        root = Path(directory)
        cases = [(fixture, (0, "", ""))]
        for name, call in negatives.items():
            source = root / f"{name}.gti"
            dispatch = f"if (false) return {call};"
            assert original.count(dispatch) == 1
            source.write_text(original.replace(dispatch, f"if (true) return {call};"))
            lines = original.splitlines()
            start = next(i for i, line in enumerate(lines, 1)
                         if line.strip().startswith(f"unsafe int {name}("))
            site = next(i for i, line in enumerate(lines, 1)
                        if i > start and "return *pointer + 1;" in line)
            cases.append((source, (70, "", f"{source.name}:{site}: "
                                  "runtime error[GTI-R0001]: integer_overflow in addition\n")))
        for label, flags, extra in modes:
            for source, expected in cases:
                binary = root / f"{source.stem}-{label}"
                built = subprocess.run([str(compiler), str(source), *flags, "-o", str(binary),
                                        "--", str(native), *extra], capture_output=True,
                                       text=True, timeout=120, env=environment)
                assert built.returncode == 0, (label, source, built.stdout, built.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        timeout=30, env=environment)
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    label, source, result)
            print(f"Returned field addresses: {label} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
