#!/usr/bin/env python3
"""Managed and scoped workers on LLVM, differentially against the C++ oracle.

Every case uses user-defined task, capture and result types. The C++ backend
supplies the oracle observable (exit status, stdout and stderr, including the
exact defined-failure reports); native LLVM must reproduce it at O0-O3 under
the concurrent execution profile.
"""

import json
import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


def fixtures():
    return {
        "managed-tasks": "llvm_thread_managed.gti",
        "managed-results": "llvm_thread_results.gti",
        "scoped-tasks": "llvm_thread_scoped.gti",
        "scoped-failure": "llvm_thread_scoped_failure.gti",
        "worker-failure": "llvm_thread_worker_failure.gti",
        "worker-emergency": "llvm_thread_worker_emergency.gti",
        "task-cleanup-emergency": "llvm_thread_task_cleanup_emergency.gti",
        "worker-tls": "llvm_thread_tls.gti",
        "worker-tls-emergency": "llvm_thread_tls_emergency.gti",
        "class-result": "managed_thread_class_result.gti",
        "scoped-cleanup": "scoped_parallel_cleanup.gti",
        "task-cleanup-failure": "panic_task_cleanup.gti",
    }


def evidence_cases(root):
    return {name: (root / "tests/fixtures" / fixture).read_text(encoding="utf-8")
            for name, fixture in fixtures().items()}


def evidence_profiles(root):
    del root
    return {"threads.managed": tuple(fixtures())}


def evidence_manifest(root):
    return build_manifest(
        "llvm_thread_execution", evidence_cases(root), evidence_profiles(root),
        {name: f"tests/fixtures/{fixture}" for name, fixture in fixtures().items()},
        execution_profiles={"threads.managed": "concurrent"})


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    gti, root = (Path(value).resolve() for value in sys.argv[1:3])
    environment = {**os.environ, "NO_COLOR": "1"}
    native = [Endpoint("llvm", f"O{level}") for level in range(4)]
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-llvm-threads-") as temporary:
        work = Path(temporary)
        for name, fixture in fixtures().items():
            source = root / "tests/fixtures" / fixture
            oracle = execute(gti, root, work / f"{name}-cpp", source,
                             Endpoint("cpp", "O0", "c++23"), environment,
                             concurrent=True, run_timeout=60)
            if oracle.error:
                raise AssertionError(f"{name}: C++ oracle failed: {oracle.error}")
            for endpoint in native:
                result = execute(gti, root,
                                 work / f"{name}-llvm-{endpoint.optimization}",
                                 source, endpoint, environment, concurrent=True,
                                 run_timeout=60)
                if result.error or result.observable != oracle.observable:
                    failures.append(
                        f"{name} {endpoint.optimization}: native {result.observable!r} "
                        f"{result.error[:400]} oracle {oracle.observable!r}")
                    continue
                print(f"{name} {endpoint.optimization}: matched", flush=True)
    if failures:
        raise AssertionError("\n".join(failures))


if __name__ == "__main__":
    main()
