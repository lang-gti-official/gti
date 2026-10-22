#!/usr/bin/env python3
"""Interleaved O3 native TLS costs, preserving the dynamic registry contract.

Resource counts cover the explicit resource allocator, not every process heap
allocation. The runtime registry is intrusive and allocates no registry nodes.
Executable sizes include each language's shipped diagnostics/runtime tables;
the native comparison does not reproduce GTI source-attribution tables.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile
import time

DECLARATIONS = """\
extern "C" {
  uint64_t gti_tls_bench_clock();
  uint64_t gti_tls_bench_iterations();
  uint64_t gti_tls_bench_step(uint64_t iteration);
  void* gti_tls_bench_acquire();
  void gti_tls_bench_release(void* value);
  void gti_tls_bench_report(uint64_t warm, uint64_t cold, uint64_t value);
}
"""
PASSIVE = "thread_local mut uint64_t counter = 0;\n"
DYNAMIC = """\
void* acquire_resource() {
  unsafe { return gti_tls_bench_acquire(); }
}
class Resource {
  void* handle = nullptr;
  mut uint64_t value = uint64_t(0);
public:
  Resource() : handle(acquire_resource()) {}
  void add(uint64_t amount) mut { this.value += amount; }
  uint64_t read() { return this.value; }
  ~Resource() { unsafe { gti_tls_bench_release(this.handle); } }
};
thread_local mut Resource resource{};
"""
BODY = """\
int main() {
  uint64_t cold_start = gti_tls_bench_clock();
  __COLD__
  uint64_t cold = gti_tls_bench_clock() - cold_start;
  uint64_t iterations = gti_tls_bench_iterations();
  uint64_t start = gti_tls_bench_clock();
  for (mut uint64_t index = uint64_t(0); index < iterations; index += uint64_t(1)) {
    __ADD__
  }
  uint64_t elapsed = gti_tls_bench_clock() - start;
  gti_tls_bench_report(elapsed, cold, __READ__);
  return 0;
}
"""


def execute(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, timeout=240)
    if result.returncode or result.stderr:
        raise AssertionError(f"{command!r}: {result.returncode}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")
    return result


def measured(command: list[str], statistics_path: pathlib.Path) -> tuple[subprocess.CompletedProcess[str], int | None]:
    timer = pathlib.Path("/usr/bin/time")
    if not timer.exists():
        return execute(command), None
    option = "-l" if sys.platform == "darwin" else "-v"
    result = execute([str(timer), option, "-o", str(statistics_path), *command])
    statistics_text = statistics_path.read_text()
    if sys.platform == "darwin":
        match = re.search(r"(\d+)\s+maximum resident set size", statistics_text)
        rss = int(match.group(1)) if match else None
    else:
        match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", statistics_text)
        rss = int(match.group(1)) * 1024 if match else None
    return result, rss


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit("usage: thread_local_performance_test.py <gti> <cxx> <runtime-library> <report.json>")
    compiler, cxx, runtime, destination = map(pathlib.Path, sys.argv[1:])
    compiler, runtime = compiler.resolve(), runtime.resolve()
    # The invocation spelling can select the C++ link driver even when c++ is
    # a symlink to clang; canonicalization belongs in provenance, not argv[0].
    cxx = cxx.absolute()
    repository = pathlib.Path(__file__).resolve().parents[1]
    fixtures = repository / "tests" / "fixtures"
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "cxx": str(cxx), "cxx_realpath": str(cxx.resolve()),
              "iterations": 20000000, "samples": 9,
              "registry_contract": "explicit first-use, recursion/shutdown rejection and same-thread drain",
              "allocation_scope": "explicit 64-byte resource allocator only; registry nodes are intrusive",
              "variants": {}}
    with tempfile.TemporaryDirectory(prefix="gti-tls-performance-") as directory:
        root = pathlib.Path(directory)
        for kind in ("passive", "dynamic"):
            source = DECLARATIONS + (PASSIVE if kind == "passive" else DYNAMIC) + BODY.replace(
                "__COLD__", "counter += uint64_t(0);" if kind == "passive" else "resource.add(uint64_t(0));").replace(
                "__ADD__", "counter += gti_tls_bench_step(index);" if kind == "passive" else "resource.add(gti_tls_bench_step(index));").replace(
                "__READ__", "counter" if kind == "passive" else "resource.read()")
            path = root / f"{kind}.gti"
            path.write_text(source)
            binaries = {name: root / f"{kind}-{name}" for name in ("gti", "cpp")}
            commands = {
                "gti": [str(compiler), str(path), "--std", "c++23", "-O3",
                        "--progress=never", "-o", str(binaries["gti"]), "--",
                        str(fixtures / "thread_local_benchmark_support.cpp")],
                "cpp": [str(cxx), "-std=c++23", "-O3", "-pthread",
                        "-I", str(repository / "runtime" / "include"),
                        str(fixtures / "thread_local_benchmark.cpp"),
                        str(fixtures / "thread_local_benchmark_support.cpp"),
                        str(runtime), "-o", str(binaries["cpp"])],
            }
            if kind == "dynamic":
                commands["cpp"].append("-DGTI_TLS_BENCH_DYNAMIC")
            data = {}
            for language, command in commands.items():
                started = time.monotonic()
                _, peak_rss = measured(command, root / f"{kind}-{language}-compile.time")
                data[language] = {"compile_seconds": time.monotonic() - started,
                                  "compile_peak_rss_bytes": peak_rss,
                                  "executable_bytes": binaries[language].stat().st_size,
                                  "warm_ns": [], "cold_ns": [], "resource_allocations": [],
                                  "runtime_peak_rss_bytes": []}
            for sample in range(9):
                order = ("gti", "cpp") if sample % 2 == 0 else ("cpp", "gti")
                for language in order:
                    observed, peak_rss = measured([str(binaries[language])],
                                                  root / f"{kind}-{language}-{sample}.time")
                    fields = list(map(int, observed.stdout.split()))
                    warm, cold, value, allocations, releases = fields
                    if value != 30000000 or allocations != (kind == "dynamic"):
                        raise AssertionError(f"{kind}/{language}: invalid complete state {fields}")
                    # GTI reports before its verified hosted drain; C++ reports
                    # after the same drain. Release counts therefore differ at
                    # this exact observation point, not in cleanup behavior.
                    expected_releases = int(kind == "dynamic" and language == "cpp")
                    if releases != expected_releases:
                        raise AssertionError(f"{kind}/{language}: invalid release count {fields}")
                    data[language]["warm_ns"].append(warm)
                    data[language]["cold_ns"].append(cold)
                    data[language]["resource_allocations"].append(allocations)
                    data[language]["runtime_peak_rss_bytes"].append(peak_rss)
            for language in data:
                data[language]["median_warm_ns"] = statistics.median(data[language]["warm_ns"])
                data[language]["median_cold_ns"] = statistics.median(data[language]["cold_ns"])
            data["warm_gti_over_cpp"] = data["gti"]["median_warm_ns"] / data["cpp"]["median_warm_ns"]
            data["cold_gti_over_cpp"] = data["gti"]["median_cold_ns"] / data["cpp"]["median_cold_ns"]
            report["variants"][kind] = data
            destination.write_text(json.dumps(report, indent=2) + "\n")
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({kind: {key: value for key, value in data.items() if key.endswith("over_cpp")}
                      for kind, data in report["variants"].items()}))


if __name__ == "__main__":
    main()
