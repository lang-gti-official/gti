#!/usr/bin/env python3
"""Backend-neutral native-size observations and storage rollback."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from backend_modes import native_modes


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    # CTest supplies the exact runtime built alongside the selected compiler.
    # Do not silently locate another installed/archive runtime for this test.
    runtime = os.environ.get("GTI_RUNTIME_LIBRARY")
    assert runtime and Path(runtime).is_file(), "GTI_RUNTIME_LIBRARY must name the selected build's runtime"
    environment = dict(os.environ,
                       GTI_STDLIB_PATH=str(root / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(root / "runtime/include"),
                       GTI_RUNTIME_LIBRARY=str(Path(runtime).resolve()))
    fixtures = [root / "tests/fixtures" / f"{name}.gti" for name in
                ("process_storage_query_policy", "process_resource_policy")]
    provider = root / "tests/fixtures/process_resource_policy_native.cpp"
    # LLVM at O0/O2/O3 under the default policy, plus the C++ profile when
    # GTI_TEST_BACKENDS selects it (tests/backend_modes.py).
    modes = [(label, flags) for label, flags, _native in native_modes(levels=("O0", "O2", "O3"))]
    with tempfile.TemporaryDirectory(prefix="gti-storage-query-") as directory:
        for name, flags in modes:
            for fixture in fixtures:
                binary = Path(directory) / f"{fixture.stem}-{name}"
                command = [str(compiler), str(fixture), *flags, "-o", str(binary),
                           "--", str(provider)]
                built = subprocess.run(command, env=environment, capture_output=True,
                                       text=True, timeout=120)
                assert built.returncode == 0, (command, built.returncode, built.stdout, built.stderr)
                result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                        text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
                    name, result.returncode, result.stdout, result.stderr)
                print(f"Process storage/provider policy: {fixture.stem} {name} passed", flush=True)


if __name__ == "__main__":
    main()
