#!/usr/bin/env python3
"""Compare optimized range patterns with their explicit field-reference equivalent."""
import json
import pathlib
import re
import resource
import statistics
import subprocess
import sys
import tempfile
import time

SOURCE = """extern "C" { uint64_t seed(); void sink(uint64_t value); }
struct Pair {
  uint64_t key;
  uint64_t value;
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


def main():
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
            pattern = SOURCE if mode == "borrowed" else SOURCE.replace("auto& [", "auto [")
            if mode == "mutable":
                pattern = SOURCE.replace("  Pair pairs", "  mut Pair pairs").replace("auto& [", "mut auto& [").replace("      state ^= key + value;", "      key += 1; value += 1; state ^= key + value;")
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
