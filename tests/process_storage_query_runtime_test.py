#!/usr/bin/env python3
"""Backend-neutral native-size observations and storage rollback."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


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
    fixture = root / "tests/fixtures/process_storage_query_policy.gti"
    provider = root / "tests/fixtures/process_resource_policy_native.cpp"
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    with tempfile.TemporaryDirectory(prefix="gti-storage-query-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            command = [str(compiler), str(fixture), *flags, "-o", str(binary),
                       "--", str(provider)]
            built = subprocess.run(command, env=environment, capture_output=True,
                                   text=True, timeout=120)
            assert built.returncode == 0, (command, built.returncode, built.stdout, built.stderr)
            result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                    text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
                name, result.returncode, result.stdout, result.stderr)
            print(f"Storage query policy: {name} passed", flush=True)


if __name__ == "__main__":
    main()
