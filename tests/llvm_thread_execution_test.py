#!/usr/bin/env python3
"""Managed and scoped workers on LLVM, differentially against the C++ oracle.

Every case uses user-defined task, capture, result and mutex payload types,
including mutex storage held by closures and owner pointees, and tasks and
mutex payloads that hold hierarchy storage. The C++ backend
supplies the oracle observable (exit status, stdout and stderr, including the
exact defined-failure reports); native LLVM must reproduce it at O0-O3 under
the concurrent execution profile.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


def managed_fixtures():
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
        "unstarted-handles": "llvm_thread_unstarted.gti",
    }


# Manual C-thread services through their exact trusted C declarations.
def cthread_fixtures():
    return {
        "cthread-create-join": "cthread_create_join.gti",
        "cthread-owned-arguments": "cthread_owned_arguments.gti",
        "cthread-mutex": "cthread_mutex.gti",
        "cthread-condition": "cthread_condition.gti",
        "cthread-sleep": "cthread_sleep_concurrent.gti",
        "cthread-identity": "cthread_identity_concurrent.gti",
        "cthread-yield": "cthread_yield_concurrent.gti",
    }


# Mutex storage composed into closure environments taken by value and into
# unique/shared owner pointees, with passive and cleanup-bearing payloads.
def mutex_composition_fixtures():
    return {
        "mutex-closures": "llvm_thread_mutex_closures.gti",
        "owned-mutexes": "llvm_thread_owned_mutexes.gti",
        "owned-mutex-failure": "llvm_thread_owned_mutex_failure.gti",
        "shared-mutex-workers": "shared_mutex_threads.gti",
        "passive-mutex-payload": "mutex_move_only.gti",
    }


# Tasks holding hierarchy storage inline (polymorphic objects, inline slots,
# erased transferable callables) in managed task boxes and scoped groups, and
# hierarchy results joined back through std::expected.
def hierarchy_task_fixtures():
    return {
        "hierarchy-tasks": "llvm_thread_hierarchy_tasks.gti",
        "hierarchy-results": "llvm_thread_hierarchy_results.gti",
        "hierarchy-carrier-tasks": "llvm_thread_hierarchy_carrier_tasks.gti",
        "transfer-function-tasks": "transfer_function_threads.gti",
        "scoped-erased-tasks": "scoped_parallel_threads.gti",
    }


# Mutex storage protecting hierarchy payloads, including moves and a shared
# owner carried to a worker.
def hierarchy_mutex_fixtures():
    return {
        "hierarchy-mutexes": "llvm_thread_hierarchy_mutexes.gti",
        "mutex-hierarchy-contract": "llvm_mutex_hierarchy_contract.gti",
    }


def fixtures():
    return {**managed_fixtures(), **cthread_fixtures(),
            **mutex_composition_fixtures(), **hierarchy_task_fixtures(),
            **hierarchy_mutex_fixtures()}


# Cases that link a test-only native helper; they run as controls because the
# helper is not GTI source. Expected status is the C++ oracle's.
NATIVE_HELPER_CONTROLS = (
    ("cthread_context.gti", "cthread_context.cpp"),
    ("cthread_nested.gti", "cthread_context.cpp"),
    ("cthread_failure.gti", "cthread_context.cpp"),
    ("cthread_entry_rejected.gti", "cthread_context.cpp"),
    ("cthread_foreign_rejected.gti", "cthread_context.cpp"),
    ("cthread_outstanding.gti", "cthread_context.cpp"),
    ("cthread_mutex_concurrent.gti", "cthread_mutex_context.cpp"),
    ("cthread_mutex_destroy.gti", "cthread_mutex_context.cpp"),
    ("cthread_condition_concurrent.gti", "cthread_condition_context.cpp"),
    ("cthread_condition_destroy.gti", "cthread_condition_context.cpp"),
    ("cthread_once.gti", "cthread_once_context.cpp"),
    ("cthread_once_failure.gti", "cthread_once_context.cpp"),
    ("cthread_once_failure_managed.gti", "cthread_once_context.cpp"),
    ("cthread_once_failure_after_return.gti", "cthread_once_context.cpp"),
    ("cthread_once_entry_rejected.gti", "cthread_once_context.cpp"),
    ("cthread_once_foreign_rejected.gti", "cthread_once_context.cpp"),
    ("cthread_once_null_flag.gti", "cthread_once_context.cpp"),
    ("cthread_once_null_initializer.gti", "cthread_once_context.cpp"),
    ("cthread_sleep_oracle.gti", "cthread_sleep_oracle.cpp"),
    ("cthread_yield_call_count.gti", "cthread_yield_call_count.cpp"),
    ("llvm_thread_join_quarantine.gti", "llvm_thread_join_fault.cpp"),
    ("llvm_contract_terminate_confined.gti", "llvm_contract_terminate.cpp"),
    ("llvm_contract_terminate_exception.gti", "llvm_contract_terminate.cpp"),
)
# Oracle anchors: an environmental automatic-join failure is terminal GTI-R0012;
# a broken native contract reaches the host's C++ terminate handler, not abort.
ORACLE_EXPECTATIONS = {
    "llvm_thread_join_quarantine.gti": (70, "GTI-R0012"),
    "llvm_contract_terminate_confined.gti": (86, "terminate-handler"),
    "llvm_contract_terminate_exception.gti": (86, "terminate-handler"),
}


def evidence_cases(root):
    return {name: (root / "tests/fixtures" / fixture).read_text(encoding="utf-8")
            for name, fixture in fixtures().items()}


def evidence_profiles(root):
    del root
    return {"threads.managed": tuple(managed_fixtures()),
            "cthread.manual": tuple(cthread_fixtures()),
            "threads.mutex-composition": tuple(mutex_composition_fixtures()),
            "threads.hierarchy-tasks": tuple(hierarchy_task_fixtures()),
            "threads.hierarchy-mutexes": tuple(hierarchy_mutex_fixtures())}


def evidence_manifest(root):
    return build_manifest(
        "llvm_thread_execution", evidence_cases(root), evidence_profiles(root),
        {name: f"tests/fixtures/{fixture}" for name, fixture in fixtures().items()},
        execution_profiles={"threads.managed": "concurrent",
                            "cthread.manual": "concurrent",
                            "threads.mutex-composition": "concurrent",
                            "threads.hierarchy-tasks": "concurrent",
                            "threads.hierarchy-mutexes": "concurrent"})


def native_helper_controls(gti, root, work, environment, cxx):
    """Linked native helpers: identical GTI source on both backends.

    Helper objects are test-only native code (runtime fault hooks, C-thread
    contexts). Native LLVM must reproduce the C++ oracle's exit status, stdout
    and exact failure reports at O0-O3.
    """
    objects = {}
    for _, helper in NATIVE_HELPER_CONTROLS:
        if helper in objects:
            continue
        objects[helper] = work / (Path(helper).stem + ".o")
        subprocess.run([cxx, "-std=c++20", "-c", str(root / "tests/fixtures" / helper),
                        "-I", str(root / "runtime/src"), "-I", str(root / "runtime/include"),
                        "-o", str(objects[helper])],
                       check=True, capture_output=True, timeout=180)
    failures = []
    for fixture, helper in NATIVE_HELPER_CONTROLS:
        source = root / "tests/fixtures" / fixture
        observed = {}
        for backend, level in (("cpp", "O0"), *(("llvm", f"O{n}") for n in range(4))):
            program = work / f"{Path(fixture).stem}-{backend}-{level}"
            built = subprocess.run([str(gti), str(source), "--backend", backend, f"-{level}",
                                    "--execution-profile", "concurrent", "-o", str(program),
                                    "--", str(objects[helper])], cwd=root, env=environment,
                                   capture_output=True, text=True, timeout=240)
            if built.returncode != 0:
                failures.append(f"{fixture} {backend} {level}: {built.stderr[-1000:]}")
                break
            ran = subprocess.run([str(program)], env=environment, capture_output=True,
                                 text=True, timeout=60)
            observed[(backend, level)] = (ran.returncode, ran.stdout, ran.stderr)
        oracle = observed.get(("cpp", "O0"))
        expected = ORACLE_EXPECTATIONS.get(fixture)
        if oracle is not None and expected and (oracle[0] != expected[0] or
                                                expected[1] not in oracle[2]):
            failures.append(f"{fixture}: oracle changed {oracle!r}")
        for key, value in observed.items():
            if value != oracle:
                failures.append(f"{fixture} {key}: {value!r} oracle {oracle!r}")
        if oracle is not None and len(observed) == 5:
            print(f"{fixture} with {helper}: matched {oracle[0]}", flush=True)
    return failures


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    gti, root = args.gti.resolve(), args.root.resolve()
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
        failures += native_helper_controls(gti, root, work, environment, args.cxx)
    if failures:
        raise AssertionError("\n".join(failures))


if __name__ == "__main__":
    main()
