#!/usr/bin/env python3

import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile


def run(
    command: list[str], environment: dict[str, str]
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
        env=environment,
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def fail_text(label: str, detail: str) -> int:
    sys.stderr.write(f"{label}: {detail}\n")
    return 1


def concrete_instances(generated: str, name: str) -> list[tuple[str, str]]:
    return re.findall(
        rf"\b(__gti_inst_[0-9a-f]{{64}}_{name}"
        rf"(?:__gti_mir_failure)?)\([^;{{]*\)\s*\{{\s*\n\s*"
        rf"// GTI verified-MIR body: [^\n]* function-instance (\d+)",
        generated,
    )


def instance_declaration(mir: str, instance: str) -> str | None:
    match = re.search(
        rf"^function @{instance} declaration=(\d+) ", mir, re.MULTILINE
    )
    return match.group(1) if match is not None else None


def emit_artifact(
    compiler: pathlib.Path,
    source: pathlib.Path,
    option: str,
    output: pathlib.Path,
    environment: dict[str, str],
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(compiler),
            str(source),
            option,
            "--std",
            "c++23",
            "-O0",
            "-o",
            str(output),
        ],
        environment,
    )


def valid_mir(artifact: str) -> bool:
    return artifact.startswith("mir-v") and " valid=1\n" in artifact[:80]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generic_algorithm_cluster_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    find_count = fixtures / "generic_algorithm_find_count.gti"
    for_each = fixtures / "generic_algorithm_for_each.gti"
    complete_range = fixtures / "complete_range_probe.gti"
    blocker = repository / "tests/fixtures/generic_algorithm_for_each_blocked.gti"
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")

    with tempfile.TemporaryDirectory(prefix="gti-generic-algorithm-") as temporary:
        root = pathlib.Path(temporary)
        artifacts: dict[str, tuple[str, str]] = {}
        for label, source in (
            ("find-count", find_count),
            ("for-each", for_each),
            ("complete-range", complete_range),
        ):
            mir_path = root / f"{label}.mir"
            emitted_mir = emit_artifact(
                compiler, source, "--emit-mir", mir_path, environment
            )
            if emitted_mir.returncode != 0:
                return fail(f"emit MIR {label}", emitted_mir)
            mir = mir_path.read_text(encoding="utf-8")
            if not valid_mir(mir):
                return fail_text(f"emit MIR {label}", "artifact is not marked valid")

            cpp_path = root / f"{label}.cpp"
            emitted_cpp = emit_artifact(
                compiler, source, "--emit-cpp", cpp_path, environment
            )
            if emitted_cpp.returncode != 0:
                return fail(f"emit C++ {label}", emitted_cpp)
            artifacts[label] = (mir, cpp_path.read_text(encoding="utf-8"))

        find_mir, find_generated = artifacts["find-count"]
        find_rows = concrete_instances(find_generated, "find")
        count_rows = concrete_instances(find_generated, "count")
        if len(find_rows) != 2 or len(count_rows) != 2:
            return fail_text(
                "backend identity",
                f"expected two concrete find/count definitions, got "
                f"find={len(find_rows)}/count={len(count_rows)}",
            )
        find_declarations = {
            instance_declaration(find_mir, instance)
            for _, instance in find_rows
        }
        count_declarations = {
            instance_declaration(find_mir, instance)
            for _, instance in count_rows
        }
        if (
            None in find_declarations
            or None in count_declarations
            or len(find_declarations) != 1
            or len(count_declarations) != 1
            or find_declarations == count_declarations
        ):
            return fail_text(
                "MIR identity",
                f"concrete definitions did not map to one distinct MIR "
                f"declaration each: find={find_declarations}/"
                f"count={count_declarations}",
            )

        # Public type aliases retain the iterator's native primary. Concrete
        # function identities and their exact MIR declaration mapping above
        # remain mandatory in either class representation.
        vector_iterator = (
            r"(?:__gti_type_[0-9a-f]{64}_vector_iterator|"
            r"::__gti_program::__gti_std::detail::vector_iterator<std::int32_t, 0>)"
        )
        find_backend_patterns = (
            r"__gti_inst_[0-9a-f]{64}_find__gti_mir_failure\("
            r"[^;]*move_only_iterator[^;]*move_only_sentinel",
            r"__gti_inst_[0-9a-f]{64}_find__gti_mir_failure\("
            rf"[^;]*{vector_iterator}[^;]*vector_sentinel",
            r"__gti_inst_[0-9a-f]{64}_count__gti_mir_failure\("
            r"[^;]*move_only_iterator[^;]*move_only_sentinel",
            r"__gti_inst_[0-9a-f]{64}_count__gti_mir_failure\("
            rf"[^;]*{vector_iterator}[^;]*vector_sentinel",
        )
        for pattern in find_backend_patterns:
            if re.search(pattern, find_generated) is None:
                return fail_text(
                    "backend identity", f"missing specialization matching {pattern!r}"
                )

        for_each_mir, for_each_generated = artifacts["for-each"]
        for_each_rows = concrete_instances(for_each_generated, "for_each")
        for_each_declarations = {
            instance_declaration(for_each_mir, instance)
            for _, instance in for_each_rows
        }
        if (
            len(for_each_rows) != 2
            or None in for_each_declarations
            or len(for_each_declarations) != 1
        ):
            return fail_text(
                "MIR identity",
                f"expected two concrete for_each definitions with one MIR "
                f"declaration, got rows={len(for_each_rows)}/"
                f"declarations={for_each_declarations}",
            )
        callable_contract_counts = {
            "combined contract": for_each_mir.count(
                "boundary=confined;transport=exact-return:"
            ),
            "read selection": for_each_mir.count("required=mut;selected=read"),
            "mutable selection": for_each_mir.count("required=mut;selected=mut"),
            "confined invocation": for_each_mir.count(
                "callable-boundary=confined"
            ),
        }
        expected_contract_counts = {
            "combined contract": 2,
            "read selection": 1,
            "mutable selection": 1,
            "confined invocation": 2,
        }
        if callable_contract_counts != expected_contract_counts:
            return fail_text(
                "callable MIR contract",
                f"expected {expected_contract_counts}, got {callable_contract_counts}",
            )

        for_each_backend_patterns = (
            r"__gti_inst_[0-9a-f]{64}_for_each__gti_mir_failure\("
            r"[^;]*iteration_iterator[^;]*iteration_sentinel[^;]*read_observer",
            r"__gti_inst_[0-9a-f]{64}_for_each__gti_mir_failure\("
            rf"[^;]*{vector_iterator}"
            r"[^;]*vector_sentinel"
            r"[^;]*mutable_observer",
        )
        for pattern in for_each_backend_patterns:
            if re.search(pattern, for_each_generated) is None:
                return fail_text(
                    "backend identity", f"missing specialization matching {pattern!r}"
                )

        complete_mir, complete_generated = artifacts["complete-range"]
        expected_complete_instances = {
            "all_of": 2,
            "fill": 2,
            "for_each": 1,
            "measured_size": 2,
        }
        for name, expected_count in expected_complete_instances.items():
            rows = concrete_instances(complete_generated, name)
            declarations = {
                instance_declaration(complete_mir, instance)
                for _, instance in rows
            }
            if (
                len(rows) != expected_count
                or None in declarations
                or len(declarations) != 1
            ):
                return fail_text(
                    "complete-range identity",
                    f"expected {expected_count} concrete {name} definitions "
                    f"with one MIR declaration, got rows={len(rows)}/"
                    f"declarations={declarations}",
                )
        if complete_mir.count("range-iterations 1") != 5:
            return fail_text(
                "complete-range MIR",
                "expected exactly five verified concrete range plans",
            )
        if " modify " in complete_mir:
            return fail_text(
                "complete-range MIR",
                "symbolic iterator modification leaked into a concrete instance",
            )

        for label, source in (
            ("find-count", find_count),
            ("for-each", for_each),
            ("complete-range", complete_range),
        ):
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O3"):
                    executable = root / f"{label}-{standard}-{optimization}"
                    built = run(
                        [
                            str(compiler),
                            str(source),
                            "--std",
                            standard,
                            f"-{optimization}",
                            "-o",
                            str(executable),
                        ],
                        environment,
                    )
                    if built.returncode != 0:
                        return fail(
                            f"build {label} {standard} {optimization}", built
                        )

                    executed = run([str(executable)], environment)
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        return fail(
                            f"run {label} {standard} {optimization}", executed
                        )

        negative_cases = (
            (
                "generic_algorithm_bad_sentinel.gti",
                "GTI-S2029",
                (
                    "satisfy concept 'std::sentinel_for",
                    "std::sentinel_for<Sentinel, Iterator>",
                ),
            ),
            (
                "generic_algorithm_bad_dereference.gti",
                "GTI-S2029",
                (
                    "satisfy concept 'std::dereferences_to",
                    "std::dereferences_to<Iterator, T>",
                ),
            ),
            (
                "generic_algorithm_bad_equality.gti",
                "GTI-S2029",
                (
                    "do not satisfy any alternative in concept 'std::equality_comparable'",
                    "Required application 'std::equality_comparable<value>'.",
                    "Provide safe, public, read-only bool == and != operators",
                ),
            ),
            (
                "generic_algorithm_for_each_missing_move.gti",
                "GTI-S2046",
                (
                    "Owned callable argument 3 requires an explicit std::move "
                    "ownership transfer.",
                    "Move an exact movable callable; any checked dependencies "
                    "must outlive its destination.",
                    "Raw implementation state must be encapsulated by an owner "
                    "with a valid unsafe transfer contract; direct raw-pointer "
                    "captures are not supported.",
                ),
            ),
            (
                "generic_algorithm_for_each_illegal_store.gti",
                "GTI-S2046",
                (
                    "Exact-return callable parameter 'function' cannot be used "
                    "as an ordinary value outside direct invocation and its "
                    "final exact move.",
                    "Invoke the callable directly and return it only through "
                    "the declaration's exact std::move transport.",
                ),
            ),
            (
                "generic_algorithm_for_each_bad_argument.gti",
                "GTI-S2029",
                (
                    "satisfy concept 'std::indirect_operation",
                    "std::indirect_operation<Function, Iterator, void>",
                    "iterator's read-only dereference result",
                ),
            ),
            (
                "generic_algorithm_for_each_bad_return.gti",
                "GTI-S2029",
                (
                    "satisfy concept 'std::indirect_operation",
                    "std::indirect_operation<Function, Iterator, void>",
                    "returning the required owned result type",
                ),
            ),
            (
                "generic_algorithm_ambiguous_return_owner.gti",
                "GTI-S2045",
                (
                    "Borrowed return dependency is ambiguous; exactly one "
                    "read-only reference or direct borrowed-value parameter may "
                    "provide its owner.",
                    "Eligible owner dependency source is here.",
                ),
            ),
        )
        # A moved owned callable may carry checked borrowed state whose
        # owners outlive its destination (borrowing callbacks); the borrowed
        # observer therefore compiles and runs under the same contract.
        borrowed = fixtures / "generic_algorithm_for_each_borrow_capture.gti"
        accepted = emit_artifact(
            compiler,
            borrowed,
            "--emit-mir",
            root / f"{borrowed.stem}.mir",
            environment,
        )
        if (
            accepted.returncode != 0
            or accepted.stdout.splitlines()[-1] != "Diagnostics: 0 errors, 0 warnings"
            or "error[" in accepted.stderr
        ):
            return fail("borrowed observer moved into for_each", accepted)
        for filename, code, fragments in negative_cases:
            source = fixtures / filename
            expected_summary = (
                "Diagnostics: 2 errors, 0 warnings\n"
                if filename == "generic_algorithm_ambiguous_return_owner.gti"
                else "Diagnostics: 1 error, 0 warnings\n"
            )
            rejected = emit_artifact(
                compiler,
                source,
                "--emit-mir",
                root / f"{source.stem}.mir",
                environment,
            )
            if (
                rejected.returncode != 65
                or rejected.stdout != expected_summary
                or f"error[{code}]" not in rejected.stderr
                or str(source) not in rejected.stderr
                or any(fragment not in rejected.stderr for fragment in fragments)
            ):
                return fail(source.stem, rejected)

        blocker_digest = hashlib.sha256(blocker.read_bytes()).hexdigest()
        expected_blocker_digest = (
            "fdad8288d757a9d4559709eaff4482d53a8a32cfa5b37d2b2ca7c9bc9ae4f982"
        )
        if blocker_digest != expected_blocker_digest:
            return fail_text(
                "for_each blocker probe",
                f"source changed: expected {expected_blocker_digest}, "
                f"got {blocker_digest}",
            )
        blocker_mir_path = root / "for-each-blocker-now-valid.mir"
        unblocked = emit_artifact(
            compiler, blocker, "--emit-mir", blocker_mir_path, environment
        )
        if unblocked.returncode != 0:
            return fail("for_each blocker probe", unblocked)
        if not valid_mir(blocker_mir_path.read_text(encoding="utf-8")):
            return fail_text(
                "for_each blocker probe", "unblocked MIR is not marked valid"
            )

        # The destination carrier crosses the algorithm's module boundary and
        # a generic move relay. Its input owner can die independently, while
        # its exclusive destination dependency survives until the result ends.
        copy_return = fixtures / "generic_algorithm_copy_return.gti"
        for backend, standard, level in (
            ("llvm", "c++23", "O0"), ("llvm", "c++23", "O1"),
            ("llvm", "c++23", "O2"), ("llvm", "c++23", "O3"),
            ("cpp", "c++20", "O3"), ("cpp", "c++23", "O3"),
        ):
            binary = root / f"copy-return-{backend}-{standard}-{level}"
            built = run([str(compiler), str(copy_return), "--backend", backend,
                         "--std", standard, f"-{level}", "-o", str(binary)], environment)
            if built.returncode:
                return fail("returned copy cursor build", built)
            executed = run([str(binary)], environment)
            if executed.returncode or executed.stdout or executed.stderr:
                return fail("returned copy cursor execution", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
