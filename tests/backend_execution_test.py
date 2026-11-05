#!/usr/bin/env python3
"""Protocol controls; real compiler/corpus tests supply execution evidence."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

from backend_execution import Endpoint, execute, policy_endpoints


def main() -> None:
    # Endpoint harnesses must honor the same selection as flag-based harnesses:
    # default LLVM, one frozen C++23/O3 profile, no implicit C++20 differential.
    for selected in ("llvm", "cpp", "both"):
        with patch.dict(os.environ, {"GTI_TEST_BACKENDS": selected}):
            endpoints = policy_endpoints()
            expected = (() if selected == "cpp" else
                        (Endpoint("llvm", "O0"), Endpoint("llvm", "O3")))
            if selected != "llvm":
                expected += (Endpoint("cpp", "O3", "c++23"),)
            assert endpoints == expected, (selected, endpoints)
    assert policy_endpoints(("O0", "O1", "O2", "O3"), backends="llvm") == tuple(
        Endpoint("llvm", level) for level in ("O0", "O1", "O2", "O3"))
    with tempfile.TemporaryDirectory(prefix="gti-backend-execution-") as raw:
        root = Path(raw)
        fixtures = {
            "associative_constructor_runtime_test.py": {
                "associative_node_constructor", "associative_include_only",
                "borrowed_temporary_operator_receiver", "dependent_carrier_representation",
                "nested_owned_field_assignment", "passive_nested_constructor_argument",
                "associative_owning_node_constructor", "associative_iterator_compound",
                "associative_owned_argument",
            },
            "complete_containers_test.py": {
                "complete_list", "complete_list_differential", "complete_deque",
                "complete_sets", "complete_containers_ownership",
                "owned_constructor_policy_forwarding", "vector_contains",
            },
            "constructor_null_native_runtime_test.py": {
                "nullable_member", "unique_member", "native_strlen", "native_qsort",
                "nullable_example", "native_identity",
            },
        }
        # Actual wrappers used to invoke the compiler before/after listing.
        # A nonexistent compiler proves this protocol is metadata-only, while
        # the exact rows ensure the second container group is not lost.
        for harness, names in fixtures.items():
            listed = subprocess.run(
                [sys.executable, str(Path(__file__).parent / harness),
                 str(root / "must-not-be-invoked-gti"), "--list-modes"],
                capture_output=True, text=True, timeout=20,
                env={**os.environ, "GTI_TEST_BACKENDS": "llvm",
                     "CC": str(root / "must-not-be-invoked-cc"),
                     "CXX": str(root / "must-not-be-invoked-cxx")})
            assert listed.returncode == 0 and not listed.stderr, (harness, listed)
            rows = [tuple(line.split("\t")) for line in listed.stdout.splitlines()]
            expected_rows = {(name, "llvm-" + level, "execute")
                             for name in names for level in ("O0", "O3")}
            assert set(rows) == expected_rows and len(rows) == len(expected_rows), (harness, rows)
        calls = []

        def run(command, **kwargs):
            calls.append((command, kwargs))
            if "--backend" in command:
                output = Path(command[command.index("-o") + 1])
                output.write_bytes(b"artifact")
                if "--keep-cpp" in command:
                    Path(f"{output}.gti.cpp").write_text("retained representation")
                return subprocess.CompletedProcess(command, 0, "build status", "")
            return subprocess.CompletedProcess(command, 70, "cleanup trace\n", "defined failure\n")

        for backend in ("cpp", "llvm"):
            endpoint = Endpoint(backend, "O3", "c++23" if backend == "cpp" else None,
                                keep_cpp=backend == "cpp")
            with patch("backend_execution.subprocess.run", side_effect=run):
                result = execute(root / "gti", root, root / backend,
                                 root / "input.gti", endpoint, dict(os.environ),
                                 concurrent=True)
            command, options = calls[-2]
            assert command[command.index("--backend") + 1] == backend
            assert "--execution-profile" in command and "-O3" in command
            assert options["cwd"] == root and options["timeout"] == 240
            assert result.observable == (70, "cleanup trace\n", "defined failure\n")
            assert result.compile_returncode == 0 and not result.error
            if backend == "llvm":
                assert "--std" not in command and "--keep-cpp" not in command
                assert result.generated_cpp is None
            else:
                assert result.generated_cpp == "retained representation"
        for endpoint in [lambda: Endpoint("llvm", "O0", "c++23"),
                         lambda: Endpoint("llvm", "O0", keep_cpp=True),
                         lambda: Endpoint("unknown", "O0")]:
            try:
                endpoint()
                raise AssertionError("an invalid endpoint was accepted")
            except ValueError:
                pass

        failed = subprocess.CompletedProcess([], 65, "", "error[GTI-B0003]: pending")
        with patch("backend_execution.subprocess.run", return_value=failed) as mocked:
            result = execute(root / "gti", root, root / "unsupported", root / "input.gti",
                             Endpoint("llvm", "O0"), dict(os.environ))
        assert mocked.call_count == 1 and result.error and result.compile_returncode == 65
        with patch("backend_execution.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            result = execute(root / "gti", root, root / "absent", root / "input.gti",
                             Endpoint("llvm", "O0"), dict(os.environ))
        assert "no executable" in result.error
    print("backend-neutral execution protocol controls passed")


if __name__ == "__main__":
    main()
