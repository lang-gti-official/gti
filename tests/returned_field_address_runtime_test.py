#!/usr/bin/env python3
"""Returned field addresses preserve values, never stale pointee proofs."""

from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


def run_fixture(compiler, filename, negatives, marker,
                native_filename="optimizer_returned_field_address_native.cpp",
                jobs=1):
    fixtures = Path(__file__).parent / "fixtures"
    fixture = fixtures / filename
    original = fixture.read_text()
    native = fixtures / native_filename
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
                         if re.match(r"(?:unsafe\s+)?(?:int|uint64_t)\s+" +
                                     re.escape(name) + r"\(", line.strip()))
            case_marker = marker[name] if isinstance(marker, dict) else marker
            site = next(i for i, line in enumerate(lines, 1)
                        if i > start and case_marker in line)
            cases.append((source, (70, "", f"{source.name}:{site}: "
                                  "runtime error[GTI-R0001]: integer_overflow in addition\n")))
        # Every (mode, source) pair is one bounded-worker case: compile and run
        # concurrently, then check results on this thread in the original
        # order. The case set, flags, expectations and timeouts are unchanged.
        plan = [(f"{label}:{source.name}", (label, flags, extra, source, expected))
                for label, flags, extra in modes for source, expected in cases]

        def work(case):
            label, flags, extra, source, expected = case
            binary = root / f"{source.stem}-{label}"
            built = subprocess.run([str(compiler), str(source), *flags, "-o", str(binary),
                                    "--", str(native), *extra], capture_output=True,
                                   text=True, timeout=120, env=environment)
            return (label, source, expected, built, binary)

        completed = {}

        # Workers only compile; each binary runs here, in plan order.
        def observe(value):
            label, source, expected, built, binary = value
            assert built.returncode == 0, (label, source, built.stdout, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    timeout=30, env=environment)
            assert (result.returncode, result.stdout, result.stderr) == expected, (
                label, source, result)
            completed[label] = completed.get(label, 0) + 1
            if completed[label] == len(cases):
                print(f"{filename}: {label} passed ({len(cases)} cases)", flush=True)

        run_cases(plan, work, jobs, observe)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="bounded concurrent compile-and-run workers")
    arguments = parser.parse_args()
    compiler = Path(arguments.compiler).resolve()
    jobs = arguments.jobs

    def run(filename, negatives, marker, *extra):
        run_fixture(compiler, filename, negatives, marker, *extra, jobs=jobs)

    run("optimizer_returned_field_address.gti", {
        "bad_different_field": "owner.bad_different_field()",
        "bad_ambiguous_field": "owner.bad_ambiguous_field(false)",
        "bad_mutating_field": "owner.bad_mutating_field()",
        "bad_opaque_field": "owner.bad_opaque_field()",
        "bad_reassigned_nested": "relay.bad_reassigned_nested()",
        "bad_opaque_nested": "relay.bad_opaque_nested()",
    }, "return *pointer + 1;")
    run("optimizer_constructed_borrows.gti", {
        "bad_borrow_ambiguous": "bad_borrow_ambiguous(storage.data(), &other, false, owner)",
        "bad_borrow_lifetime": "bad_borrow_lifetime(storage.data(), owner)",
        "bad_borrow_different": "bad_borrow_different(storage.data(), &other, owner)",
        "bad_borrow_opaque": "bad_borrow_opaque(storage.data(), owner)",
        "bad_borrow_write": "bad_borrow_write(storage.data(), owner)",
    }, "return value + 1;")
    run("optimizer_indexed_borrows.gti", {
        "bad_index_mismatch": "bad_index_mismatch(&pair[0], uint64_t(0), uint64_t(1), owner)",
        "bad_index_hole": "bad_index_hole(&pair[0], owner)",
    }, "return value + 1;")
    run("optimizer_result_postconditions.gti", {
        "bad_failed_ticket": "bad_failed_ticket(ticket)",
        "bad_reassigned_ticket": "bad_reassigned_ticket(ticket)",
        "bad_later_call": "bad_later_call(ticket)",
        "bad_native_call": "bad_native_call(ticket)",
        "bad_cached_tag": "bad_cached_tag(ticket)",
    }, "return ticket.count + 1;")
    run("optimizer_passive_result_cleanup.gti", {
        "bad_active_result": "counter.bad_active_result()",
    }, "return this.count + 1;")
    run("optimizer_pointer_field_contents.gti", {
        "bad_changed": "cell.bad_changed()",
        "bad_native": "cell.bad_native()",
        "bad_retarget": "cell.bad_retarget(&other)",
        "bad_global": "cell.bad_global()",
        "bad_borrowed": "cell.bad_borrowed()",
        "bad_lifetime": "cell.bad_lifetime()",
    }, "return *this.address + 1;", "optimizer_pointer_field_contents_native.cpp")
    run("optimizer_sibling_field_publication.gti", {
        "bad_sibling_native": "ledger.bad_sibling_native()",
        "bad_sibling_indirect": "ledger.bad_sibling_indirect()",
        "bad_sibling_borrow": "ledger.bad_sibling_borrow()",
        "bad_sibling_direct": "ledger.bad_sibling_direct()",
    }, "return this.datum.value + 1;")
    run("optimizer_checked_address_store.gti", {
        "bad_checked_overflow": "bad_checked_overflow(first.data(), owner)",
        "bad_checked_native": "bad_checked_native(first.data(), owner)",
        "bad_checked_other": "bad_checked_other(first.data(), &other, owner)",
        "bad_checked_call": "bad_checked_call(first.data(), owner)",
    }, " + 1;")
    run("optimizer_typed_scalar_stores.gti", {
        "bad_same_width": "int(data.bad_same_width())",
        "bad_native_width": "int(data.bad_native_width())",
        "bad_raw_width": "int(data.bad_raw_width())",
        "bad_lifetime_width": "int(data.bad_lifetime_width())",
    }, "return this.wide + uint64_t(1);", "optimizer_typed_scalar_stores_native.cpp")
    run("optimizer_pointer_field_snapshots.gti", {
        "bad_snapshot_retarget": "bad_snapshot_retarget()",
        "bad_snapshot_native": "bad_snapshot_native()",
        "bad_snapshot_raw": "bad_snapshot_raw()",
        "bad_snapshot_loop": "bad_snapshot_loop()",
        "bad_snapshot_conditional": "bad_snapshot_conditional(true)",
    }, "return *pointer + 1;")
    run("optimizer_vacant_sequence.gti", {
        "bad_vacant_replaced": "bad_vacant_replaced(first.data(), second.data())",
        "bad_vacant_native": "bad_vacant_native(first.data(), second.data())",
        "bad_vacant_lifetime": "bad_vacant_lifetime(first.data(), second.data())",
    }, "return *pointer + 1;")
    run("optimizer_record_pointees.gti", {
        "bad_record_broad": "bad_record_broad(storage.data(), owner)",
        "bad_record_native": "bad_record_native(storage.data(), owner)",
        "bad_record_field_borrow": "bad_record_field_borrow(storage.data(), owner)",
        "bad_record_lifetime": "bad_record_lifetime(storage.data(), owner)",
        "bad_record_argument": "bad_record_argument(storage.data(), owner)",
        "bad_record_other": "bad_record_other(storage.data(), &other, owner)",
    }, "return value.narrow + 1;")
    run("optimizer_prefix_frontier.gti", {
        "bad_prefix_native": "bad_prefix_native(sequence)",
        "bad_prefix_retarget": "bad_prefix_retarget(sequence, &other)",
        "bad_prefix_gap": "bad_prefix_gap()",
        "bad_prefix_partial": "bad_prefix_partial()",
        "bad_prefix_value": "bad_prefix_value()",
        "bad_prefix_lifetime": "bad_prefix_lifetime(sequence)",
    }, "return value + 1;")
    run("optimizer_default_scalar_construction.gti", {
        "bad_default_explicit": "bad_default_explicit(first.data())",
        "bad_default_native": "bad_default_native(first.data())",
        "bad_default_other": "bad_default_other(first.data(), second.data())",
        "bad_default_lifetime": "bad_default_lifetime(first.data())",
        "bad_modified_prefix": "bad_modified_prefix(sequence)",
        "bad_modified_native": "bad_modified_native(sequence)",
        "bad_compound_prefix": "bad_compound_prefix(sequence)",
        "bad_compound_native": "bad_compound_native(sequence)",
    }, {name: (" += 1;" if name.startswith("bad_compound_") else " + 1;")
        for name in ("bad_default_explicit", "bad_default_native",
                     "bad_default_other", "bad_default_lifetime",
                     "bad_modified_prefix", "bad_modified_native",
                     "bad_compound_prefix", "bad_compound_native")})
    run("optimizer_record_prefix.gti", {
        "bad_record_prefix_broad": "bad_record_prefix_broad(sequence)",
        "bad_record_prefix_store": "bad_record_prefix_store(sequence)",
        "bad_record_prefix_native": "bad_record_prefix_native(sequence)",
        "bad_record_prefix_constructor": "bad_record_prefix_constructor(sequence)",
    }, " + 1;")


if __name__ == "__main__":
    main()
