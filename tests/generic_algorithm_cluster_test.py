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


def declaration_id(generated: str, name: str) -> str | None:
    match = re.search(
        rf"\b__gti_fn_(\d+)_{name}\(Iterator\s+__gti_mir_arg_0,\s*"
        rf"Sentinel\s+__gti_mir_arg_1,",
        generated,
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
    blocker = repository / "tests/fixtures/generic_algorithm_for_each_blocked.gti"
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")

    with tempfile.TemporaryDirectory(prefix="gti-generic-algorithm-") as temporary:
        root = pathlib.Path(temporary)
        artifacts: dict[str, tuple[str, str]] = {}
        for label, source in (("find-count", find_count), ("for-each", for_each)):
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
        find_declaration = declaration_id(find_generated, "find")
        count_declaration = declaration_id(find_generated, "count")
        if find_declaration is None or count_declaration is None:
            return fail_text(
                "backend identity", "generic find/count declarations are absent"
            )

        find_instances = re.findall(
            rf"^function @\d+ declaration={find_declaration} ",
            find_mir,
            re.MULTILINE,
        )
        count_instances = re.findall(
            rf"^function @\d+ declaration={count_declaration} ",
            find_mir,
            re.MULTILINE,
        )
        if len(find_instances) != 2 or len(count_instances) != 2:
            return fail_text(
                "MIR identity",
                f"expected find=2/count=2, got "
                f"find={len(find_instances)}/count={len(count_instances)}",
            )

        find_backend_patterns = (
            rf"__gti_fn_{find_declaration}_find__gti_mir_failure\("
            r"[^;]*move_only_iterator[^;]*move_only_sentinel",
            rf"__gti_fn_{find_declaration}_find__gti_mir_failure\("
            r"[^;]*vector_iterator<std::int32_t>[^;]*vector_sentinel",
            rf"__gti_fn_{count_declaration}_count__gti_mir_failure\("
            r"[^;]*move_only_iterator[^;]*move_only_sentinel",
            rf"__gti_fn_{count_declaration}_count__gti_mir_failure\("
            r"[^;]*vector_iterator<std::int32_t>[^;]*vector_sentinel",
        )
        for pattern in find_backend_patterns:
            if re.search(pattern, find_generated) is None:
                return fail_text(
                    "backend identity", f"missing specialization matching {pattern!r}"
                )

        for_each_mir, for_each_generated = artifacts["for-each"]
        for_each_declaration = declaration_id(for_each_generated, "for_each")
        if for_each_declaration is None:
            return fail_text("backend identity", "generic for_each is absent")
        for_each_instances = re.findall(
            rf"^function @\d+ declaration={for_each_declaration} ",
            for_each_mir,
            re.MULTILINE,
        )
        if len(for_each_instances) != 2:
            return fail_text(
                "MIR identity",
                f"expected for_each=2, got for_each={len(for_each_instances)}",
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
            rf"__gti_fn_{for_each_declaration}_for_each__gti_mir_failure\("
            r"[^;]*iteration_iterator[^;]*iteration_sentinel[^;]*read_observer",
            rf"__gti_fn_{for_each_declaration}_for_each__gti_mir_failure\("
            r"[^;]*vector_iterator<std::int32_t>[^;]*vector_sentinel"
            r"[^;]*mutable_observer",
        )
        for pattern in for_each_backend_patterns:
            if re.search(pattern, for_each_generated) is None:
                return fail_text(
                    "backend identity", f"missing specialization matching {pattern!r}"
                )

        for label, source in (("find-count", find_count), ("for-each", for_each)):
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
                    "Generic arguments do not satisfy concept 'std::sentinel_for'.",
                    "std::sentinel_for<Sentinel, Iterator>",
                ),
            ),
            (
                "generic_algorithm_bad_dereference.gti",
                "GTI-S2029",
                (
                    "Generic arguments do not satisfy concept 'std::dereferences_to'.",
                    "std::dereferences_to<Iterator, T>",
                ),
            ),
            (
                "generic_algorithm_bad_equality.gti",
                "GTI-S2029",
                (
                    "does not satisfy generic constraint 'std::equality_comparable'",
                    "Constraint on generic parameter 'T' is declared here.",
                ),
            ),
            (
                "generic_algorithm_for_each_missing_move.gti",
                "GTI-S2046",
                (
                    "Owned callable argument 3 requires an explicit std::move "
                    "ownership transfer.",
                    "Move an exact movable callable whose stored state contains no "
                    "references, tracked borrowed state, or raw pointers.",
                ),
            ),
            (
                "generic_algorithm_for_each_borrow_capture.gti",
                "GTI-S2046",
                (
                    "Owned callable argument 3 contains borrowed or raw-pointer "
                    "capture state.",
                    "Exact owned callable parameter declared here.",
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
                "GTI-S2022",
                (
                    "No exact overload of operator() for argument types (int32_t).",
                    "Candidate: operator()(uint64_t&)",
                    "Concrete generic instance requested here.",
                ),
            ),
            (
                "generic_algorithm_for_each_bad_return.gti",
                "GTI-S2046",
                (
                    "Callable parameter 'function' must return 'void'",
                    "selected callable returns 'int32_t'",
                    "Concrete generic instance requested here.",
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
        for filename, code, fragments in negative_cases:
            source = fixtures / filename
            rejected = emit_artifact(
                compiler,
                source,
                "--emit-mir",
                root / f"{source.stem}.mir",
                environment,
            )
            if (
                rejected.returncode != 65
                or rejected.stdout != ""
                or f"error[{code}]" not in rejected.stderr
                or str(source) not in rejected.stderr
                or any(fragment not in rejected.stderr for fragment in fragments)
            ):
                return fail(source.stem, rejected)

        blocker_digest = hashlib.sha256(blocker.read_bytes()).hexdigest()
        expected_blocker_digest = (
            "693dac7b1e5a68f7d84510bba5626dada34b255ac178bb16e3085c27616fc449"
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
