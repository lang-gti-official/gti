#!/usr/bin/env python3
"""Pending O-LLVM-08/O-LLVM-09 owned-environment composition proof."""

from pathlib import Path
import os
import sys
import tempfile

from backend_execution import Endpoint, execute, require_cpp_wrapping_rejection


SUCCESS = "llvm_callable_owned_composition"
FAILURES = {
    "llvm_callable_owned_composition_argument_failure":
        "owned callable later argument failed",
}
LLVM_ONLY_FAILURES = {
    "llvm_callable_owned_composition_wrapper_failure":
        "owned callable later initializer failed",
}
CPP = Endpoint("cpp", "O3", "c++23")
LLVM = tuple(Endpoint("llvm", level) for level in ("O0", "O1", "O2", "O3"))


def run_case(gti: Path, root: Path, work: Path, name: str, endpoint: Endpoint,
             env: dict[str, str]):
    result = execute(
        gti,
        root,
        work / f"{name}-{endpoint.backend}-{endpoint.optimization}",
        root / "tests" / "fixtures" / f"{name}.gti",
        endpoint,
        env,
    )
    if result.error:
        raise AssertionError(f"{name} {endpoint}: {result.error}")
    return result


def main() -> None:
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    oracle_only = "--oracle-only" in sys.argv[3:]
    endpoints = LLVM
    env = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="gti-llvm-callable-owned-") as raw:
        work = Path(raw)
        for name in (SUCCESS, *FAILURES, *LLVM_ONLY_FAILURES):
            require_cpp_wrapping_rejection(gti, root / "tests/fixtures" / f"{name}.gti",
                                          work / (name + "-unsupported"), env, CPP)
        if oracle_only:
            print("C++ source-owner oracle unavailable: exact wrapping rejection verified; no execution claim")
            return
        success = [run_case(gti, root, work, SUCCESS, endpoint, env)
                   for endpoint in endpoints]
        if any(result.observable != (0, "", "") for result in success):
            raise AssertionError(f"owned callable success drift: {success}")

        for name, diagnostic in FAILURES.items():
            results = [run_case(gti, root, work, name, endpoint, env)
                       for endpoint in endpoints]
            oracle = results[0].observable
            if oracle[0] != 70 or diagnostic not in oracle[2]:
                raise AssertionError(f"invalid production failure oracle: {results[0]}")
            if any(result.observable != oracle for result in results[1:]):
                raise AssertionError(f"owned callable failure drift: {results}")

        if not oracle_only:
            for name, diagnostic in LLVM_ONLY_FAILURES.items():
                results = [run_case(gti, root, work, name, endpoint, env)
                           for endpoint in LLVM]
                expected = results[0].observable
                if expected[0] != 70 or diagnostic not in expected[2]:
                    raise AssertionError(
                        f"invalid native owned-wrapper failure: {results[0]}"
                    )
                if any(result.observable != expected for result in results[1:]):
                    raise AssertionError(
                        f"native owned-wrapper failure drift: {results}"
                    )

    mode = "C++23 oracle" if oracle_only else "LLVM O0-O3 composition, C++ explicitly unsupported"
    print(f"O-LLVM-08/O-LLVM-09 owned callable {mode} passed")


if __name__ == "__main__":
    main()
