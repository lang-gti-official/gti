#!/usr/bin/env python3
"""Source-defined atomic policy, native operations and retained pending workers."""

from pathlib import Path
import os
import json
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


def cases() -> dict[str, str]:
    return {
        "scalar": "atomic_scalar.gti",
        "wrapping": "llvm_atomic_wrapping.gti",
        "wrapping-single": "llvm_atomic_wrapping.gti",
        "wrapping-example-single": "../../examples/178-wrapping-atomic-counters.gti",
        "scalar-single": "atomic_scalar.gti",
        "values-single": "llvm_atomic_values.gti",
        "orders-single": "llvm_atomic_orders.gti",
        "example-single": "../../examples/176-profile-independent-atomics.gti",
        "values": "llvm_atomic_values.gti",
        "orders": "llvm_atomic_orders.gti",
        "wait-ready": "llvm_atomic_wait_ready.gti",
        "checked-fetch": "atomic_fetch.gti",
        "overflow-before-update": "llvm_atomic_overflow.gti",
        # Required whole-milestone cases, not optional passing skips when the
        # scalar component is being validated with an explicit selector.
        "worker-contention": "atomic_concurrent.gti",
        "worker-publication": "atomic_orders.gti",
        "wait-notify": "atomic_wait.gti",
    }


def profiles() -> dict[str, list[str]]:
    return {
        "atomic.wrapping": ["wrapping"],
        "atomic.wrapping-single": ["wrapping-single", "wrapping-example-single"],
        "atomic.scalar-core": ["scalar", "values", "orders"],
        "atomic.scalar-single": ["scalar-single", "values-single", "orders-single", "example-single"],
        "atomic.wait-ready": ["wait-ready"],
        "atomic.checked-fetch": ["checked-fetch", "overflow-before-update"],
        "atomic.blocking-workers": ["worker-contention", "worker-publication", "wait-notify"],
    }


def evidence_cases(root: Path) -> dict[str, str]:
    return {
        name: (root / "tests/fixtures" / fixture).read_text(encoding="utf-8")
        for name, fixture in cases().items()
    }


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return {name: tuple(sources) for name, sources in profiles().items()}


def evidence_manifest(root: Path) -> dict:
    return build_manifest(
        "llvm_atomic_execution", evidence_cases(root), evidence_profiles(root),
        {name: f"tests/fixtures/{fixture}" for name, fixture in cases().items()},
        execution_profiles={
            "atomic.scalar-core": "concurrent",
            "atomic.wrapping": "concurrent",
            "atomic.wrapping-single": "single-threaded",
            "atomic.scalar-single": "single-threaded",
            "atomic.wait-ready": "concurrent",
            "atomic.checked-fetch": "concurrent",
            "atomic.blocking-workers": "concurrent",
        })


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else None
    selected = set(profiles().get(selection, [selection])) if selection else None
    env = dict(os.environ, GTI_STDLIB_DIR=str(root / "stdlib"))
    rejects = {
        "floating-domain": "std::atomic<float> value{1.0};",
        "enum-domain": "std::atomic<E> value{E::first};",
        "illegal-load": "std::atomic<int32_t> value{0}; [[discard]] value.load(std::memory_order_release());",
        "illegal-store": "std::atomic<int32_t> value{0}; value.store(1, std::memory_order_acquire());",
        "illegal-wait-release": "std::atomic<int32_t> value{0}; value.wait(0, std::memory_order_release());",
        "illegal-wait-acq-rel": "std::atomic<int32_t> value{0}; value.wait(0, std::memory_order_acq_rel());",
        "illegal-failure-order": "std::atomic<int32_t> value{0}; [[discard]] value.compare_exchange_strong(0, 1, std::memory_order_acq_rel(), std::memory_order_release());",
        "bool-fetch": "std::atomic<bool> value{false}; [[discard]] value.fetch_add(true);",
    }
    if selection and selection not in cases() and selection not in rejects and selection not in profiles():
        raise ValueError(f"unknown atomic case: {selection}")
    endpoints = (Endpoint("cpp", "O3", "c++20"),
                 Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", mode) for mode in ("O0", "O1", "O2", "O3")))
    with tempfile.TemporaryDirectory(prefix="gti-llvm-atomic-") as directory:
        work = Path(directory)
        for name, fixture in cases().items():
            if selected is not None and name not in selected:
                continue
            results = []
            for endpoint in endpoints:
                result = execute(gti, root,
                                 work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}",
                                 root / "tests/fixtures" / fixture, endpoint,
                                 env, concurrent=not name.endswith("-single"))
                if name.startswith("wrapping") and endpoint.backend == "cpp":
                    if (result.compile_returncode != 65 or "GTI-B0003" not in result.stderr
                            or "wrapping atomic read-modify-write" not in result.stderr):
                        raise AssertionError(f"unexpected wrapping comparator result: {result}")
                    print(f"atomic.{name}: recorded {endpoint.standard} unsupported wrapping operation", flush=True)
                    continue
                failure = name == "overflow-before-update"
                if result.error or result.returncode != (70 if failure else 0):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                if failure and ("GTI-R0001" not in result.stderr or
                                "integer_overflow in addition" not in result.stderr or
                                "GTI-R0014" in result.stderr):
                    raise AssertionError(f"overflow update/cleanup contract: {result}")
                results.append(result.observable)
            if any(result != results[0] for result in results):
                raise AssertionError(f"{name}: backend observable drift: {results}")
            scope = "LLVM O0-O3 passed; C++ limitation recorded" if name.startswith("wrapping") else "C++20/23 O3 and LLVM O0-O3 passed"
            print(f"atomic.{name}: {scope}", flush=True)
        for name, body in rejects.items():
            if selected is not None and name not in selected:
                continue
            source = work / f"{name}.gti"
            declaration = "enum class E { first };\n" if name == "enum-domain" else ""
            source.write_text("#include <std/atomic>\n" + declaration +
                              "int main() { " + body + " return 0; }\n")
            for backend in ("cpp", "llvm"):
                output = work / f"rejected-{name}-{backend}"
                output.write_bytes(b"preserved")
                result = subprocess.run(
                    [str(gti), str(source), "--backend", backend,
                     "--execution-profile", "concurrent", "-o", str(output)],
                    cwd=root, env=env, capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "error[GTI-S" not in result.stderr or
                        output.read_bytes() != b"preserved"):
                    raise AssertionError(f"{name} {backend}: {result.stderr}")
            print(f"atomic.{name}: source rejection preserves artifact", flush=True)


if __name__ == "__main__":
    main()
