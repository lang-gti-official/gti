#!/usr/bin/env python3
"""Callable execution, cleanup, borrow and optimized-cost evidence."""

from pathlib import Path
import os
import statistics
import subprocess
import sys
import tempfile
import time

from backend_execution import Endpoint, execute


SUCCESS_CASES = (
    "llvm_callable_direct",
    "llvm_callable_pointer",
    "llvm_callable_ref",
    "llvm_callable_owned",
    "function_item_pack_contracts",
    "lambda_pack_invocation",
    "nested_callable_results",
    "issue_117_nested_lambda",
    "llvm_callable_field_initializer",
    "llvm_erased_class_reference",
    "checked_callable_transport",
)
LLVM_ONLY_SUCCESS_CASES = ("llvm_callable_wrapper",)
FAILURE_CASES = {
    "llvm_callable_failure": ("GTI-R0017", "later callable argument failed"),
    "llvm_callable_pointer_failure": ("GTI-R0002", "division_by_zero"),
    "llvm_callable_cleanup_failure": (
        "GTI-R0017",
        "callable capture cleanup failed",
    ),
}
LLVM_ONLY_FAILURE_CASES = {
    "llvm_callable_wrapper_failure": (
        "GTI-R0017",
        "later callable wrapper initializer failed",
    ),
}
ENDPOINTS = (
    Endpoint("cpp", "O3", "c++23"),
    Endpoint("llvm", "O0"),
    Endpoint("llvm", "O1"),
    Endpoint("llvm", "O2"),
    Endpoint("llvm", "O3"),
)
LLVM_ENDPOINTS = ENDPOINTS[1:]


def run_case(
    gti: Path,
    root: Path,
    work: Path,
    name: str,
    env: dict[str, str],
    endpoints=ENDPOINTS,
):
    source = root / "tests" / "fixtures" / f"{name}.gti"
    results = []
    for endpoint in endpoints:
        result = execute(
            gti,
            root,
            work / f"{name}-{endpoint.backend}-{endpoint.optimization}",
            source,
            endpoint,
            env,
        )
        if result.error:
            raise AssertionError(f"{name} {endpoint}: {result}")
        results.append(result)
    return results


def allocation_free_object(gti: Path, root: Path, work: Path, env: dict[str, str]) -> int:
    source = root / "tests" / "fixtures" / "llvm_callable_owned.gti"
    output = work / "owned-o3"
    built = subprocess.run(
        [str(gti), str(source), "--backend", "llvm", "-O3", "-o", str(output)],
        cwd=root,
        env=env,
        text=True,
        capture_output=True,
        check=False,
        timeout=240,
    )
    if built.returncode != 0 or not output.is_file():
        raise AssertionError(f"optimized owned callable did not build: {built}")
    # The runtime archive has unrelated process services. An ordinary closure
    # must not make any of GTI's allocation providers reachable from its final
    # executable; its environment stays in caller-owned object storage.
    symbols = subprocess.run(
        ["nm", str(output)],
        cwd=root,
        env=env,
        text=True,
        capture_output=True,
        check=False,
        timeout=30,
    )
    if symbols.returncode != 0:
        raise AssertionError(f"could not inspect optimized callable: {symbols.stderr}")
    forbidden = (
        "gti_rt_aligned_malloc_v1",
        "gti_rt_posix_memalign_v1",
        "gti_rt_aligned_free_v1",
        "gti_rt_posix_free_v1",
    )
    if any(symbol in symbols.stdout for symbol in forbidden):
        raise AssertionError("ordinary closure unexpectedly retained an allocation provider")
    return output.stat().st_size


def cost_evidence(gti: Path, root: Path, work: Path, env: dict[str, str]):
    observations = {}
    for family, fixture in {
        "direct": "llvm_callable_cost_direct",
        "closure": "llvm_callable_cost_closure",
        "pointer": "llvm_callable_cost_pointer",
        "mixed": "llvm_callable_cost",
    }.items():
        observations[family] = {}
        for endpoint in (
            Endpoint("cpp", "O3", "c++23"),
            Endpoint("llvm", "O3"),
        ):
            source = root / "tests" / "fixtures" / f"{fixture}.gti"
            result = execute(
                gti,
                root,
                work / f"cost-{family}-{endpoint.backend}",
                source,
                endpoint,
                env,
            )
            if result.error or result.observable != (0, "", ""):
                raise AssertionError(
                    f"optimized {family} callable cost fixture {endpoint}: {result}"
                )
            executable = work / f"cost-{family}-{endpoint.backend}" / "program"
            samples = []
            for _ in range(5):
                start = time.perf_counter_ns()
                ran = subprocess.run(
                    [str(executable)],
                    cwd=root,
                    env=env,
                    capture_output=True,
                    check=False,
                    timeout=15,
                )
                samples.append(time.perf_counter_ns() - start)
                if ran.returncode != 0 or ran.stdout or ran.stderr:
                    raise AssertionError(f"cost execution failed: {ran}")
            observations[family][endpoint.backend] = (
                statistics.median(samples),
                executable.stat().st_size,
            )
    return observations


def main() -> None:
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    env = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="gti-llvm-callables-") as raw:
        work = Path(raw)
        for name in SUCCESS_CASES:
            results = run_case(gti, root, work, name, env)
            if any(result.observable != (0, "", "") for result in results):
                raise AssertionError(f"{name}: execution mismatch: {results}")

        for name in LLVM_ONLY_SUCCESS_CASES:
            results = run_case(gti, root, work, name, env, LLVM_ENDPOINTS)
            if any(result.observable != (0, "", "") for result in results):
                raise AssertionError(f"{name}: native execution mismatch: {results}")

        for name, diagnostics in FAILURE_CASES.items():
            results = run_case(gti, root, work, name, env)
            oracle = results[0].observable
            if oracle[0] != 70 or any(text not in oracle[2] for text in diagnostics):
                raise AssertionError(f"{name}: invalid production oracle: {results[0]}")
            if any(result.observable != oracle for result in results[1:]):
                raise AssertionError(f"{name}: native failure drift: {results}")

        for name, diagnostics in LLVM_ONLY_FAILURE_CASES.items():
            results = run_case(gti, root, work, name, env, LLVM_ENDPOINTS)
            expected = results[0].observable
            if expected[0] != 70 or any(text not in expected[2] for text in diagnostics):
                raise AssertionError(f"{name}: invalid native failure: {results[0]}")
            if any(result.observable != expected for result in results[1:]):
                raise AssertionError(f"{name}: native failure drift: {results}")

        rejection = root / "tests" / "fixtures" / "llvm_callable_borrow_reject.gti"
        preserved = work / "rejected"
        preserved.write_bytes(b"preserve")
        result = subprocess.run(
            [str(gti), str(rejection), "--backend", "llvm", "-O3", "-o", str(preserved)],
            cwd=root,
            env=env,
            text=True,
            capture_output=True,
            check=False,
            timeout=60,
        )
        if (
            result.returncode != 65
            or "GTI-S2045" not in result.stderr
            or preserved.read_bytes() != b"preserve"
        ):
            raise AssertionError(f"borrow rejection drift: {result}")

        owned_size = allocation_free_object(gti, root, work, env)
        costs = cost_evidence(gti, root, work, env)
    print(
        "LLVM callable C++23/O0-O3 execution, capture transfer, cleanup, "
        f"borrow rejection and failure parity passed; owned={owned_size}B; "
        f"cost={costs}"
    )


if __name__ == "__main__":
    main()
