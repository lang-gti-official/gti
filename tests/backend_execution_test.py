#!/usr/bin/env python3
"""Protocol controls; real compiler/corpus tests supply execution evidence."""

import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from backend_execution import Endpoint, execute


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="gti-backend-execution-") as raw:
        root = Path(raw)
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
