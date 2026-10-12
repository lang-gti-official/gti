#!/usr/bin/env python3
"""Cost control: receiver-free interface statics versus ordinary class statics.

Timing is evidence, not a noisy hard-coded pass/fail threshold. Both GTI
variants preserve checked arithmetic; the C++ control has the same success
domain and explicit overflow checks. No allocation or virtual-dispatch
optimization is claimed by enabling the declaration spelling.
"""

import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


GTI = """TYPE Ops {
public:
  static uint64_t step(uint64_t value) {
    return (value * 17 + 11) % 1000003;
  }
};
int main() {
  mut uint64_t state = 1;
  for (mut int index = 0; index < 5000000; index++) {
    state = Ops::step(state);
  }
  std::println(state);
  return 0;
}
"""
CPP = """#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
class Ops {
public:
  static std::uint64_t step(std::uint64_t value) {
    if (value > (std::numeric_limits<std::uint64_t>::max() - 11) / 17)
      std::exit(101);
    return (value * 17 + 11) % 1000003;
  }
};
int main() {
  std::uint64_t state = 1;
  for (int index = 0; index < 5000000; ++index) state = Ops::step(state);
  std::printf("%llu\\n", static_cast<unsigned long long>(state));
}
"""


def checked(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError((argv, result.stdout[-4000:], result.stderr[-4000:]))
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    evidence = Path(sys.argv[2]).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    native = shutil.which("clang++") or shutil.which("c++")
    assert native, "native C++ compiler is required"
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "compiler_version": checked([str(compiler), "--version"]).stdout.strip(),
              "native_version": checked([native, "--version"]).stdout.splitlines()[0],
              "workload": "5000000 receiver-free static scalar transforms", "variants": {}}
    with tempfile.TemporaryDirectory(prefix="gti-interface-static-cost-") as temp:
        directory = Path(temp)
        binaries = {}
        for kind in ("class", "interface", "cpp"):
            source = directory / (kind + (".cpp" if kind == "cpp" else ".gti"))
            text = CPP if kind == "cpp" else GTI.replace("TYPE", kind)
            source.write_text(text)
            binary = directory / kind
            argv = ([native, "-std=c++23", "-O3", str(source), "-o", str(binary)]
                    if kind == "cpp" else
                    [str(compiler), str(source), "--std", "c++23", "-O3", "-o", str(binary)])
            before = time.perf_counter()
            built = checked((["/usr/bin/time", "-l"] if sys.platform == "darwin" else []) + argv)
            data = {"source_sha256": hashlib.sha256(text.encode()).hexdigest(),
                    "build_seconds": time.perf_counter() - before,
                    "binary_bytes": binary.stat().st_size, "runs_seconds": []}
            if sys.platform == "darwin":
                data["build_peak_rss_bytes"] = next(
                    int(line.split()[0]) for line in built.stderr.splitlines()
                    if "maximum resident set size" in line)
            if kind != "cpp":
                emitted = evidence / f"{kind}.gti.cpp"
                checked([str(compiler), str(source), "--std", "c++23", "-O3", "--emit-cpp", "-o", str(emitted)])
                data["emitted_cpp_bytes"] = emitted.stat().st_size
            if shutil.which("otool"):
                disassembly = checked(["otool", "-tvV", str(binary)]).stdout
                (evidence / f"{kind}.disassembly.txt").write_text(disassembly)
            report["variants"][kind] = data
            binaries[kind] = binary
        expected = None
        for repetition in range(10):
            kinds = ("class", "interface", "cpp") if repetition % 2 == 0 else ("cpp", "interface", "class")
            for kind in kinds:
                before = time.perf_counter()
                run = checked([str(binaries[kind])])
                duration = time.perf_counter() - before
                expected = run.stdout if expected is None else expected
                assert (run.stdout, run.stderr) == (expected, ""), (kind, run)
                if repetition >= 3:
                    report["variants"][kind]["runs_seconds"].append(duration)
        report["complete_state_output"] = expected.strip()
        for variant in report["variants"].values():
            variant["median_seconds"] = statistics.median(variant["runs_seconds"])
        report["interface_over_class_ratio"] = (
            report["variants"]["interface"]["median_seconds"] /
            report["variants"]["class"]["median_seconds"])
    (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"interface_over_class_ratio": report["interface_over_class_ratio"],
                      "output": report["complete_state_output"],
                      "evidence": str(evidence / "report.json")}))


if __name__ == "__main__":
    main()
