#!/usr/bin/env python3
"""Record optimized ownership-transfer costs; allocation and cleanup are asserted in GTI."""
import argparse
import json
import pathlib
import resource
import re
import sys
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-consuming-cost-") as folder:
        binary = pathlib.Path(folder) / "cost"
        start = time.perf_counter()
        built = subprocess.run([str(args.compiler.resolve()),
            str(root / "tests/fixtures/consuming_range_cost.gti"),
            "--backend", "llvm", "-O3", "-o", str(binary), "--",
            str(root / "tests/fixtures/process_resource_policy_native.cpp")],
            capture_output=True, text=True, timeout=120)
        assert built.returncode == 0, built
        result = {"compile_seconds": time.perf_counter() - start,
                  "compile_child_maxrss_native_units": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
                  "binary_bytes": binary.stat().st_size,
                  "elements_per_sample": 2048 * 65536,
                  "allocations_per_vector": 1,
                  "releases_per_vector": 1}
        samples = {"borrowed": [], "consuming": []}
        # Alternate ordering and discard one warmup for each mode. Both paths
        # fill, sum, and destroy the same number of noncopyable elements.
        for repetition in range(8):
            modes = ("borrowed", "consuming") if repetition % 2 else ("consuming", "borrowed")
            for mode in modes:
                arguments = ["large", "workload"] + (["borrowed"] if mode == "borrowed" else [])
                start = time.perf_counter()
                run = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=30)
                duration = time.perf_counter() - start
                assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), run
                if repetition:
                    samples[mode].append(duration)
        result["samples_seconds"] = samples
        result["median_seconds"] = {mode: statistics.median(times) for mode, times in samples.items()}
        result["consuming_to_borrowed_ratio"] = result["median_seconds"]["consuming"] / result["median_seconds"]["borrowed"]
        print(json.dumps(result, indent=2))



# Structured pattern cases share the range harness and its registration.
STRUCTURED_SOURCE = """extern "C" { uint64_t seed(); void sink(uint64_t value); }
struct Pair {
  mut uint64_t key;
  mut uint64_t value;
  Pair(uint64_t key, uint64_t value) : key(key), value(value) {}
};
uint64_t run(uint64_t initial) {
  Pair pairs[2] = {Pair(initial, uint64_t(2)), Pair(uint64_t(3), uint64_t(4))};
  mut uint64_t state = initial;
  for (mut int i = 0; i < 4000000; i++) {
    for (auto& [key, value] : pairs) {
      state ^= key + value;
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
    }
  }
  return state;
}
int main() { unsafe { sink(run(seed())); } return 0; }
"""


def invoke(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result


def measure_structured_patterns():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-range-pattern-cost-") as directory:
        root = pathlib.Path(directory)
        native = root / "seed.c"
        native.write_text('#include <stdint.h>\n#include <stdio.h>\nvolatile uint64_t initial = 7;\nuint64_t seed(void) { return initial; }\nvoid sink(uint64_t value) { printf("%llu\\n", (unsigned long long)value); }\n')
        obj = root / "seed.o"
        invoke(["cc", "-O3", "-c", str(native), "-o", str(obj)])
        report = {}
        binaries = {}
        for mode in ("borrowed", "owned", "mutable"):
            pattern = STRUCTURED_SOURCE if mode == "borrowed" else STRUCTURED_SOURCE.replace("auto& [", "auto [")
            if mode == "mutable":
                pattern = STRUCTURED_SOURCE.replace("  Pair pairs", "  mut Pair pairs").replace("auto& [", "mut auto& [").replace("      state ^= key + value;", "      key += 1; value += 1; state ^= key + value;")
            explicit = pattern.replace("[key, value]", "entry").replace(
                " : pairs) {", " : pairs) { auto& key = entry.key; auto& value = entry.value;")
            if mode == "mutable":
                explicit = explicit.replace("auto& key", "mut auto& key").replace("auto& value", "mut auto& value")
            for shape, source in (("pattern", pattern), ("explicit", explicit)):
                name = mode + "-" + shape
                path = root / (name + ".gti")
                path.write_text(source)
                binary = root / name
                ir = root / (name + ".ll")
                start = time.perf_counter()
                invoke([compiler, str(path), "-O3", "-o", str(binary), "--", str(obj)])
                seconds = time.perf_counter() - start
                invoke([compiler, str(path), "-O3", "--emit-llvm", "-o", str(ir)])
                allocation_calls = [line for line in ir.read_text().splitlines()
                                    if re.search(r"\bcall\b.*(?:malloc|calloc|realloc|allocate)", line)]
                assert not allocation_calls, (name, allocation_calls)
                binaries[name] = binary
                report[name] = {"compile_seconds": seconds, "binary_bytes": binary.stat().st_size,
                                "ir_bytes": ir.stat().st_size, "allocation_calls": 0, "samples": []}
        outputs = {mode: set() for mode in ("borrowed", "owned", "mutable")}
        for repetition in range(8):
            names = list(binaries)
            if repetition % 2:
                names.reverse()
            for name in names:
                start = time.perf_counter()
                output = invoke([str(binaries[name])])
                elapsed = time.perf_counter() - start
                outputs[name.split("-")[0]].add(output.stdout)
                if repetition:
                    report[name]["samples"].append(elapsed)
        assert all(len(values) == 1 for values in outputs.values()), outputs
        for values in report.values():
            values["median_seconds"] = statistics.median(values["samples"])
        report["compiler_child_maxrss_native_units"] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        for mode in ("borrowed", "owned", "mutable"):
            report[mode + "_pattern_to_explicit_ratio"] = (
                report[mode + "-pattern"]["median_seconds"] /
                report[mode + "-explicit"]["median_seconds"])
        print(json.dumps(report, indent=2))



if __name__ == "__main__":
    main()
    measure_structured_patterns()
