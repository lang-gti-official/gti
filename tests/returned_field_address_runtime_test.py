#!/usr/bin/env python3
"""Returned field addresses preserve values, never stale pointee proofs."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


def run_fixture(compiler, filename, negatives, marker):
    fixtures = Path(__file__).parent / "fixtures"
    fixture = fixtures / filename
    original = fixture.read_text()
    native = fixtures / "optimizer_returned_field_address_native.cpp"
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
                         if line.strip().startswith((f"unsafe int {name}(", f"int {name}(")))
            site = next(i for i, line in enumerate(lines, 1)
                        if i > start and marker in line)
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
            print(f"{filename}: {label} passed ({len(cases)} cases)", flush=True)


def main():
    compiler = Path(sys.argv[1]).resolve()
    run_fixture(compiler, "optimizer_returned_field_address.gti", {
        "bad_different_field": "owner.bad_different_field()",
        "bad_ambiguous_field": "owner.bad_ambiguous_field(false)",
        "bad_mutating_field": "owner.bad_mutating_field()",
        "bad_opaque_field": "owner.bad_opaque_field()",
        "bad_reassigned_nested": "relay.bad_reassigned_nested()",
        "bad_opaque_nested": "relay.bad_opaque_nested()",
    }, "return *pointer + 1;")
    run_fixture(compiler, "optimizer_constructed_borrows.gti", {
        "bad_borrow_ambiguous": "bad_borrow_ambiguous(storage.data(), &other, false, owner)",
        "bad_borrow_lifetime": "bad_borrow_lifetime(storage.data(), owner)",
        "bad_borrow_different": "bad_borrow_different(storage.data(), &other, owner)",
        "bad_borrow_opaque": "bad_borrow_opaque(storage.data(), owner)",
        "bad_borrow_write": "bad_borrow_write(storage.data(), owner)",
    }, "return value + 1;")
    run_fixture(compiler, "optimizer_indexed_borrows.gti", {
        "bad_index_mismatch": "bad_index_mismatch(&pair[0], uint64_t(0), uint64_t(1), owner)",
        "bad_index_hole": "bad_index_hole(&pair[0], owner)",
    }, "return value + 1;")
    run_fixture(compiler, "optimizer_result_postconditions.gti", {
        "bad_failed_ticket": "bad_failed_ticket(ticket)",
        "bad_reassigned_ticket": "bad_reassigned_ticket(ticket)",
        "bad_later_call": "bad_later_call(ticket)",
        "bad_native_call": "bad_native_call(ticket)",
        "bad_cached_tag": "bad_cached_tag(ticket)",
    }, "return ticket.count + 1;")
    run_fixture(compiler, "optimizer_passive_result_cleanup.gti", {
        "bad_active_result": "counter.bad_active_result()",
    }, "return this.count + 1;")


if __name__ == "__main__":
    main()
