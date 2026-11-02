#!/usr/bin/env python3
"""Typed relocation preserves endpoint coverage, failures and cleanup."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from internal_module_fixture import private_library, private_source, register_modules


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
        (library / "std/prefix_query_probe.gti").write_text(
            (root / "tests/fixtures/optimizer_prefix_query_probe.gti").read_text())
        register_modules(library, {"prefix_query_probe": "std/prefix_query_probe.gti"})
        environment = dict(os.environ, GTI_STDLIB_PATH=str(library))
        for mode, flags in modes:
            binary = Path(directory) / mode
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in range(24):
                result = subprocess.run([str(binary), *(["x"] * case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == (0 if case in (0, 5, 9, 10, 11, 13, 14, 15, 18, 20, 23) else 70), (mode, case, result)
                assert result.stdout == "99\n", (mode, case, result.stdout, result.stderr)
                if case not in (0, 5, 9, 10, 11, 13, 14, 15, 18, 20, 23):
                    assert "runtime error[GTI-R" in result.stderr and "GTI-R0014" not in result.stderr, (mode, case, result.stderr)
                else:
                    assert not result.stderr, (mode, result.stderr)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0":
                    observations[case] = observation
                else:
                    assert observation == observations[case], (mode, case, observation, observations[case])
            endpoint_source = private_source(
                root / "tests/fixtures/optimizer_prefix_endpoints.gti",
                Path(directory) / "optimizer_prefix_endpoints.gti",
                ["detail.memory_storage"])
            endpoint_binary = Path(directory) / (mode + "-endpoints")
            command = [str(compiler), str(endpoint_source), *flags, "-o", str(endpoint_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for endpoint_case in range(7):
                result = subprocess.run([str(endpoint_binary), *(["x"] * endpoint_case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                failure = endpoint_case == 2
                assert result.returncode == (70 if failure else 0), (mode, endpoint_case, result)
                assert result.stdout == "99\n", (mode, endpoint_case, result)
                if failure:
                    assert "runtime error[GTI-R" in result.stderr and "GTI-R0014" not in result.stderr
                else:
                    assert not result.stderr, (mode, endpoint_case, result)
                key = ("endpoint", endpoint_case)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0":
                    observations[key] = observation
                else:
                    assert observation == observations[key], (mode, key, observation, observations[key])
            concurrent_source = private_source(
                root / "tests/fixtures/optimizer_prefix_receiver_concurrent.gti",
                Path(directory) / "optimizer_prefix_receiver_concurrent.gti",
                ["detail.memory_storage"])
            concurrent_binary = Path(directory) / (mode + "-concurrent")
            command = [str(compiler), str(concurrent_source), *flags,
                       "--execution-profile", "concurrent", "-o", str(concurrent_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for concurrent_case in range(6):
                failure = concurrent_case in (1, 3, 5)
                result = subprocess.run([str(concurrent_binary), *(["x"] * concurrent_case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == (70 if failure else 0), (mode, failure, result)
                assert result.stdout == "99\n", (mode, failure, result)
                if failure:
                    assert "runtime error[GTI-R" in result.stderr and "GTI-R0014" not in result.stderr
                else:
                    assert not result.stderr, (mode, result)
                key = ("concurrent", concurrent_case)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0":
                    observations[key] = observation
                else:
                    assert observation == observations[key], (mode, observation, observations[key])
            print(f"{mode}: successful transfer, changed value, retired source, vacant capacity and failure controls passed", flush=True)


if __name__ == "__main__":
    main()
