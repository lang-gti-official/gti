#!/usr/bin/env python3
"""Typed relocation preserves endpoint coverage, failures and cleanup."""
from pathlib import Path
import os
import json
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
        # Isolate reserve from unrelated callers: the ordinary CLI pass order
        # must prove the zero-count path, not rely on a widened call context.
        reserve_source = Path(directory) / "reserve-control.gti"
        reserve_source.write_text("""import std.vector;
int main() {
  mut std::vector<int32_t> values{};
  values.reserve(uint64_t(8));
  return int32_t(values.size() * uint64_t(18446744073709551615));
}
""")
        report = Path(directory) / "reserve-control.json"
        command = [str(compiler), str(reserve_source), "-O3", "--emit-mir",
                   "--optimization-report", str(report), "-o", str(report.with_suffix(".mir"))]
        built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
        assert built.returncode == 0, (command, built.stdout, built.stderr)
        evidence = json.loads(report.read_text())
        assert evidence["verification"]["inputValid"] and evidence["verification"]["outputValid"]
        assert evidence["safetyOperations"]["families"]["arithmetic-overflow"]["staticallyDischarged"] == 1, evidence["safetyOperations"]
        for mode, flags in modes:
            fresh_text = (root / "tests/fixtures/optimizer_fresh_storage.gti").read_text()
            fresh_module = ("export module fresh_storage_probe;\n"
                            'import "detail/memory_storage.gti";\n'
                            'import "detail/memory_capabilities.gti";\n' + fresh_text.replace(
                                "int main(int argc, std::vector<std::string> argv)",
                                "export int run_fresh_probe(int argc)"))
            (library / "std/fresh_storage_probe.gti").write_text(fresh_module)
            register_modules(library, {"fresh_storage_probe": "std/fresh_storage_probe.gti"})
            fresh_source = Path(directory) / "fresh-main.gti"
            fresh_source.write_text("import std.fresh_storage_probe; import std.vector; import std.string;\n"
                                    "int main(int argc, std::vector<std::string> argv) { return run_fresh_probe(argc); }\n")
            fresh_binary = Path(directory) / (mode + "-fresh")
            command = [str(compiler), str(fresh_source), *flags, "-o", str(fresh_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for failure in (False, True):
                result = subprocess.run([str(fresh_binary), *(["overflow"] if failure else [])],
                                        env=environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == (70 if failure else 0), (mode, failure, result)
                assert result.stdout == "99\n", (mode, failure, result)
                if failure:
                    assert "runtime error[GTI-R0001]: integer_overflow in multiplication" in result.stderr
                else:
                    assert not result.stderr, (mode, failure, result)
                key = ("fresh", failure)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0": observations[key] = observation
                else: assert observation == observations[key], (mode, key, observation, observations[key])
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
            scalar_source = root / "tests/fixtures/optimizer_equal_scalar_store.gti"
            scalar_binary = Path(directory) / (mode + "-equal-scalar")
            command = [str(compiler), str(scalar_source), *flags, "-o", str(scalar_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for scalar_case in range(7):
                result = subprocess.run([str(scalar_binary), *(["x"] * scalar_case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                failure = scalar_case in (2, 3, 5, 6)
                assert result.returncode == (70 if failure else 0), (mode, scalar_case, result)
                assert result.stdout == "99\n", (mode, scalar_case, result)
                if failure:
                    assert "runtime error[GTI-R0001]: integer_overflow in multiplication" in result.stderr
                else:
                    assert not result.stderr, (mode, scalar_case, result)
                key = ("equal-scalar", scalar_case)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0": observations[key] = observation
                else: assert observation == observations[key], (mode, key, observation, observations[key])
            endpoint_source = private_source(
                root / "tests/fixtures/optimizer_prefix_endpoints.gti",
                Path(directory) / "optimizer_prefix_endpoints.gti",
                ["detail.memory_storage"])
            endpoint_binary = Path(directory) / (mode + "-endpoints")
            command = [str(compiler), str(endpoint_source), *flags, "-o", str(endpoint_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for endpoint_case in range(11):
                result = subprocess.run([str(endpoint_binary), *(["x"] * endpoint_case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                failure = endpoint_case in (2, 8, 10)
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
            cross_source = private_source(
                root / "tests/fixtures/optimizer_prefix_cross_field.gti",
                Path(directory) / "optimizer_prefix_cross_field.gti",
                ["detail.memory_storage"])
            cross_binary = Path(directory) / (mode + "-cross-field")
            command = [str(compiler), str(cross_source), *flags, "-o", str(cross_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for cross_case in range(2):
                result = subprocess.run([str(cross_binary), *(["x"] * cross_case)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == (70 if cross_case else 0), (mode, cross_case, result)
                assert result.stdout == "99\n", (mode, cross_case, result)
                if cross_case:
                    assert "runtime error[GTI-R0001]: integer_overflow in addition" in result.stderr
                    assert "GTI-R0014" not in result.stderr
                else:
                    assert not result.stderr, (mode, cross_case, result)
                key = ("cross-field", cross_case)
                observation = result.returncode, result.stdout, result.stderr
                if mode == "llvm-O0": observations[key] = observation
                else: assert observation == observations[key], (mode, key, observation, observations[key])
            concurrent_source = private_source(
                root / "tests/fixtures/optimizer_prefix_receiver_concurrent.gti",
                Path(directory) / "optimizer_prefix_receiver_concurrent.gti",
                ["detail.memory_storage"])
            concurrent_binary = Path(directory) / (mode + "-concurrent")
            command = [str(compiler), str(concurrent_source), *flags,
                       "--execution-profile", "concurrent", "-o", str(concurrent_binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for concurrent_case in range(8):
                failure = concurrent_case in (1, 3, 5, 7)
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
