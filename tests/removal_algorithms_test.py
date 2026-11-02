#!/usr/bin/env python3
"""Stable compaction, erasure, ownership, and structural predicate contracts."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repo = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repo / "stdlib"))
    evidence = []
    with tempfile.TemporaryDirectory(prefix="gti-removal-") as temporary:
        root = pathlib.Path(temporary)

        def run(command):
            result = subprocess.run(command, env=environment, text=True,
                                    capture_output=True, timeout=240)
            return result

        for backend, optimization in (("llvm", "O0"), ("llvm", "O3"), ("cpp", "O3")):
            binary = root / f"removal-{backend}-{optimization}"
            start = time.monotonic()
            # C++ cannot represent the full mixed generic ownership fixture:
            # its uninitialized_storage trait arguments disagree. Preserve
            # the supported scalar/string comparison; LLVM owns full lifecycle evidence.
            fixture = "removal_algorithms" if backend == "llvm" else "removal_algorithms_oracle"
            built = run([str(compiler), str(repo / f"tests/fixtures/{fixture}.gti"),
                         "--backend", backend, f"-{optimization}", "-o", str(binary)])
            assert built.returncode == 0, built.stdout + built.stderr
            compile_seconds = time.monotonic() - start
            start = time.monotonic()
            result = run([str(binary)])
            assert result.returncode == 0, f"{backend}-{optimization}: exit {result.returncode}\n{result.stdout}{result.stderr}"
            assert not result.stdout and not result.stderr, result.stdout + result.stderr
            evidence.append(dict(backend=backend, optimization=optimization,
                                 compile_seconds=compile_seconds,
                                 run_seconds=time.monotonic() - start,
                                 binary_bytes=binary.stat().st_size))

        preamble = """import std.algorithm;
import std.vector;
import std.array;
import std.map;
class Once {
public:
  Once() = default;
  bool operator()(int& value) && { return true; }
};
"""
        cases = {
            "wrong-result": "std::erase_if(values, [](int& v) -> int { return v; });",
            "wrong-element": "std::erase_if(values, [](double& v) -> bool { return true; });",
            "mutable-element": "std::erase_if(values, [](mut int& v) -> bool { v = 0; return true; });",
            "consuming-predicate": "std::erase_if(values, Once());",
            "aliased-value": "int& key = values[std::size_t(0)]; std::erase(values, key);",
            "aliased-remove-value": "int& key = values[std::size_t(0)]; auto end = std::remove(values, key);",
            "retained-cursor": "auto cursor = values.begin(); std::erase_if(values, [](int& v) -> bool { return true; }); return *cursor;",
            "fixed-size-erase": "mut std::array<int, 2> fixed{}; std::erase_if(fixed, [](int& v) -> bool { return true; });",
            "immutable-sequence": "std::vector<int> frozen = {1}; auto end = std::remove_if(frozen, [](int& v) -> bool { return true; });",
            "mutable-map-entry": "mut std::map<int, int> entries{}; std::erase_if(entries, [](mut std::map<int, int>::value_type& e) -> bool { e.second = 0; return true; });",
        }
        for name, body in cases.items():
            source = root / f"{name}.gti"
            source.write_text(preamble + "int main() { mut std::vector<int> values = {1, 2}; " + body + " return 0; }\n")
            result = run([str(compiler), str(source), "--emit-mir", "-o", str(root / "rejected.mir")])
            diagnostics = result.stdout + result.stderr
            assert result.returncode != 0 and "error[GTI-S" in diagnostics, f"{name}: {diagnostics}"
            assert "error[GTI-P" not in diagnostics and "error[GTI-B" not in diagnostics, diagnostics
        print(json.dumps({"runtime": evidence, "negative_cases": len(cases)}, indent=2))
        for name in ("cost", "failure"):
            binary = root / name
            command = [str(compiler), str(repo / f"tests/fixtures/removal_algorithms_{name}.gti"),
                       "--backend", "llvm", "-O3", "-o", str(binary)]
            if name == "cost":
                command += ["--", str(repo / "tests/fixtures/stdlib_adoption_allocation_counts.cpp")]
            result = run(command)
            assert result.returncode == 0, result.stdout + result.stderr
            start = time.monotonic()
            result = run([str(binary)])
            if name == "cost":
                assert result.returncode == 0, result.stdout + result.stderr
                print(json.dumps({"cost_seconds": time.monotonic() - start,
                                  "binary_bytes": binary.stat().st_size,
                                  "elements": 65536, "iterations": 16,
                                  "removal_allocations": 0}))
            else:
                assert result.returncode != 0 and "removal predicate failed" in result.stderr, result.stdout + result.stderr
                assert sorted(result.stdout.splitlines()) == ["0", "1", "2", "3", "4"], result.stdout


if __name__ == "__main__":
    main()
