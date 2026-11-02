#!/usr/bin/env python3
"""Typed relocation preserves endpoint coverage, failures and cleanup."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from internal_module_fixture import private_library, private_source


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parent / "fixtures/optimizer_prefix_relocation.gti"
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    observations = {}
    with tempfile.TemporaryDirectory(prefix="gti-prefix-relocation-") as directory:
        root = Path(__file__).resolve().parent.parent
        library = private_library(root, Path(directory) / "stdlib", ["detail.memory_storage"])
        source = private_source(source, Path(directory) / source.name, ["detail.memory_storage"])
        environment = dict(os.environ, GTI_STDLIB_PATH=str(library))
        for mode, flags in modes:
            binary = Path(directory) / mode
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in range(9):
                result = subprocess.run([str(binary), *(["x"] * case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == (0 if case in (0, 5) else 70), (mode, case, result)
                assert result.stdout == "99\n", (mode, case, result.stdout, result.stderr)
                if case not in (0, 5):
                    assert "runtime error[GTI-R" in result.stderr and "GTI-R0014" not in result.stderr, (mode, case, result.stderr)
                else:
                    assert not result.stderr, (mode, result.stderr)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0":
                    observations[case] = observation
                else:
                    assert observation == observations[case], (mode, case, observation, observations[case])
            print(f"{mode}: successful transfer, changed value, retired source, vacant capacity and failure controls passed", flush=True)


if __name__ == "__main__":
    main()
