#!/usr/bin/env python3
"""No fixture loses its last execution mode: every runtime harness runs on LLVM.

The shipped backend gets the execution coverage (AGENTS.md, "Backend
Development Policy"). A harness that hard-codes a C++ mode table is how a
fixture silently stops executing on LLVM, so this check inventories every
registered Python harness that receives the `gti` executable and requires one
of these:

* it shares `lambda_pack_invocation_runtime_test.main`: `--list-modes` must
  plan an LLVM execution for every fixture under the default policy, or
  declare the fixture `llvm_unsupported` (an exact rejection or a filed bug
  symptom that the harness asserts still reproduces);
* it builds its modes from `tests/backend_modes.py` and spells no backend
  itself: no `--std`, no `--backend cpp`, no bare `--keep-cpp` (the LLVM
  backend rejects it; `keep_cpp(flags)` is empty there);
* it selects no backend at all, so the compiler's default, LLVM, executes it;
* or it is a C++-only entry by name (`scripts/validation_checks.py`
  `CPP_ONLY_PREFIXES`), which only the scheduled C++ job runs;
* or it is listed in ALLOWED_SPELLINGS with the reason the spelling is the
  contract under test (a CLI option check, say) rather than a mode table.

Usage: execution_coverage_test.py <repository> <build-directory>
"""

import ast
import pathlib
import re
import subprocess
import sys
import tempfile

from test_throughput_registration_test import inventory

# Harnesses whose `--std`/`--backend cpp`/`--keep-cpp` spelling is the driver
# or tooling contract they test, or a C++ differential oracle beside the LLVM
# modes they are about: never a fixture's only execution. Each entry names the
# reason; an entry that no registered test uses fails the check.
ALLOWED_SPELLINGS = {
    "annotated_test_suite_test.py": "the `gti test --std` project command is the contract under test",
    "backend_execution_test.py": "unit test of the backend endpoint protocol's own flag spelling",
    "benchmark_compare_test.py": "the benchmark driver's per-backend flag spelling is the contract under test",
    "cli_progress_test.py": "asserts the CLI help text names the C++ backend options",
    "cli_smoke_test.py": "the direct-compilation CLI contract, including --std and --keep-cpp acceptance",
    "compiled_library_project_test.py": "asserts the project driver rejects --backend cpp for a compiled library",
    "feature_interaction_model_test.py": "the model's recorded observations name the C++ options",
    "general_object_layout_test.py": "layout oracle comparing the C++ text's object layout with the LLVM layout",
    "llvm_backend_cli_test.py": "asserts the LLVM backend rejects --std and --keep-cpp",
    "llvm_cache_project_test.py": "asserts the project driver rejects --std with --backend llvm",
    "llvm_case_context_test.py": "unit test of the recorded case-context command spelling",
    "llvm_embedding_project_test.py": "asserts the embedding project rejects --backend cpp",
    "macos_installed_runtime_test.py": "installed-toolchain contract exercising the C++ options of the shipped CLI",
    "module_cpp_boundary_test.py": "the C++ module boundary of the C++ backend is the contract under test",
    "native_fixture_build_test.py": "unit test of the native provider build helper's C++ emission path",
    "project_cli_smoke_test.py": "the project CLI contract, including --keep-cpp acceptance",
    "registry_cli_test.py": "the `gti run --std` registry workflow is the contract under test",
    # LLVM-backend harnesses with a C++ differential oracle.
    "llvm_checked_range_runtime_test.py": "LLVM-backend harness; C++ endpoint is a differential oracle",
    "llvm_checked_reduction_runtime_test.py": "LLVM-backend harness; C++ endpoint is a differential oracle",
    "llvm_dispatch_execution_test.py": "LLVM-backend harness; C++ endpoints are a differential oracle",
    "llvm_hosted_cleanup_evidence_test.py": "LLVM-backend harness; the C++ standard names the recorded evidence",
    "llvm_hosted_cleanup_execution_test.py": "LLVM-backend harness; C++ endpoint is a differential oracle",
    "llvm_hosted_execution_test.py": "LLVM-backend harness; C++ endpoints are a differential oracle",
    "llvm_mutex_service_test.py": "LLVM-backend harness; C++ guard build is a differential oracle",
    "llvm_native_c_aggregate_oracle_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_native_c_callback_execution_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_native_c_confined_execution_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_native_c_contract_oracle_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_native_c_outbound_boundary_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_native_c_storage_oracle_test.py": "LLVM native-C harness; C++ entries are a differential oracle",
    "llvm_raw_pointer_array_test.py": "LLVM-backend harness; C++ entries are a differential oracle",
    "llvm_simd_source_test.py": "LLVM-backend harness; C++ entries are a differential oracle",
    # Thread-sanitizer evidence is carried by the C++ text today.
    "managed_thread_tsan_test.py": "spells --backend cpp only for the source-owner rejection pin; every ThreadSanitizer run instruments the LLVM emission (#196)",
}


def spells_a_backend(text):
    """True when the harness selects the C++ backend for an executable itself.

    `--std` inside a command that also carries `--emit-cpp` is a text probe
    of the C++ emitter, not an execution mode, and is not counted.
    """
    tree = ast.parse(text)
    probes = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.List, ast.Tuple, ast.Call)):
            constants = [n for n in ast.walk(node) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
            if any(n.value == "--emit-cpp" for n in constants):
                probes.update(id(n) for n in constants if n.value == "--std")
    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
            continue
        if node.value == "--keep-cpp":
            return True
        if node.value == "--std" and id(node) not in probes:
            return True
    for node in ast.walk(tree):
        if isinstance(node, (ast.List, ast.Tuple)):
            values = [n.value for n in node.elts if isinstance(n, ast.Constant)]
            if any(a == "--backend" and b == "cpp" for a, b in zip(values, values[1:])):
                return True
    return False


def cpp_only(repository, name):
    sys.path.insert(0, str(repository / "scripts"))
    import validation_checks  # noqa: E402  (the shared CI policy module)
    return validation_checks.cpp_only(name)


SELF = pathlib.Path(__file__).resolve()


def harness_of(test):
    """The harness file a registration runs, whether it receives the compiler, and its argv.

    Only a file named `gti` counts as the compiler: on CI the checkout itself
    is a directory named `gti`, which this test receives as its repository
    argument, and treating it as the compiler made the test list its own
    modes recursively until it timed out (CI #447).
    """
    command = test.get("command", [])
    harness = next((pathlib.Path(arg) for arg in command if arg.endswith("_test.py")), None)
    if harness is not None and harness.resolve() == SELF:
        return None, False, command
    compiler = any(pathlib.Path(arg).name in ("gti", "gti.exe") and not pathlib.Path(arg).is_dir()
                   for arg in command)
    return harness, compiler, command


def list_modes(harness, command):
    """Rows (fixture, label, plan) a shared-main harness plans under the default policy."""
    index = command.index(str(harness))
    argv = [sys.executable, str(harness), *command[index + 1:], "--list-modes"]
    result = subprocess.run(argv, capture_output=True, text=True, timeout=120,
                            env={**dict(__import__("os").environ), "GTI_TEST_BACKENDS": "llvm"})
    if result.returncode != 0:
        raise AssertionError(f"{harness.name}: --list-modes failed\n{result.stdout}{result.stderr}")
    rows = []
    for line in result.stdout.splitlines():
        if line.count("\t") != 2:
            continue  # a harness's own banner, not a plan row
        name, label, plan = line.split("\t", 2)
        rows.append((name, label, plan))
    return rows


def check_harness_of():
    with tempfile.TemporaryDirectory() as directory:
        checkout = pathlib.Path(directory) / "gti"
        checkout.mkdir()
        own = {"command": [sys.executable, str(SELF), str(checkout), str(checkout / "build")]}
        assert harness_of(own) == (None, False, own["command"]), "the coverage test must never list itself"
        other = {"command": [sys.executable, str(checkout / "x_test.py"), str(checkout), str(checkout / "build/gti")]}
        harness, compiler, _ = harness_of(other)
        assert harness == checkout / "x_test.py" and compiler, "a compiler file beside a directory named gti"
        directory_only = {"command": [sys.executable, str(checkout / "x_test.py"), str(checkout)]}
        assert harness_of(directory_only)[1] is False, "a directory named gti is not the compiler"


def main():
    check_harness_of()
    repository = pathlib.Path(sys.argv[1]).resolve()
    build = pathlib.Path(sys.argv[2]).resolve()
    tests = inventory(build)
    names = {test["name"] for test in tests}
    stale = sorted(name for name in ALLOWED_SPELLINGS
                   if not (repository / "tests" / name).is_file()
                   or not spells_a_backend((repository / "tests" / name).read_text(errors="replace")))
    if stale:
        raise AssertionError(f"ALLOWED_SPELLINGS names harnesses that no longer spell a backend: {stale}")
    violations = []
    declared = []
    counts = {"shared": 0, "policy": 0, "default-backend": 0, "cpp-only": 0, "allowed": 0}
    seen = set()
    for test in sorted(tests, key=lambda test: test["name"]):
        harness, compiler, command = harness_of(test)
        if harness is None or not compiler or not harness.is_relative_to(repository / "tests"):
            continue
        if cpp_only(repository, test["name"]):
            counts["cpp-only"] += 1
            continue
        text = harness.read_text(errors="replace")
        if re.search(r"^from lambda_pack_invocation_runtime_test import", text, re.M):
            if harness in seen:
                continue
            seen.add(harness)
            counts["shared"] += 1
            fixtures = {}
            for name, label, plan in list_modes(harness, command):
                fixtures.setdefault(name, []).append((label, plan))
            if not fixtures:
                violations.append(f"{test['name']}: plans no fixture")
            for name, plans in fixtures.items():
                executes = any(label.startswith("llvm-") and plan == "execute" for label, plan in plans)
                limitation = [plan for label, plan in plans if plan.startswith("unsupported:")]
                if limitation:
                    declared.append(f"{test['name']}/{name}: {limitation[0]}")
                elif not executes:
                    violations.append(f"{test['name']}/{name}: no LLVM execution mode and no llvm_unsupported declaration")
            continue
        if harness.name != "lambda_pack_invocation_runtime_test.py" and (
                "LLVM_UNSUPPORTED" in text or "llvm_unsupported=" in text):
            declared.append(f"{test['name']}: standalone llvm_unsupported declaration in {harness.name}")
        if spells_a_backend(text):
            if harness.name in ALLOWED_SPELLINGS:
                counts["allowed"] += 1
                continue
            violations.append(f"{test['name']} ({harness.name}): spells a backend itself; "
                              "build its modes from tests/backend_modes.py")
            continue
        if re.search(r"^from backend_modes import|^import backend_modes", text, re.M):
            counts["policy"] += 1
        else:
            counts["default-backend"] += 1
    print("execution coverage:", ", ".join(f"{key}={value}" for key, value in counts.items()))
    for line in declared:
        print("declared limitation:", line)
    if violations:
        raise AssertionError("fixtures without LLVM execution coverage:\n" + "\n".join(violations))
    print(f"every runtime harness executes on LLVM under the default policy; {len(declared)} declared limitations")


if __name__ == "__main__":
    main()
