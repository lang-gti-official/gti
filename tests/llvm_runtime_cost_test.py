#!/usr/bin/env python3
"""Correctness and opt-in reserved-host costs for the currently admitted families.

Both comparison endpoints compile GTI with the same checked contract. The C
helper is an untimed checksum/effect oracle, never a runtime-performance baseline.
Pending families are explicit obligations, not skips counted as successful tests.
"""

import argparse
from contextlib import contextmanager, ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time
import resource
import sys


@contextmanager
def measurement_window(root, args):
    if not args.measure:
        yield
        return
    sys.path.insert(0, str(root / "scripts"))
    import build_window
    window = build_window.Window(build_window.directory_for(repo=root), timeout=1)
    holders = window.status()
    expected_suite = []
    if args.parent_suite:
        expected_suite = holders["suite"]
        assert len(expected_suite) == 1 and expected_suite[0]["pid"] == os.getppid(), (
            "managed parent must own the sole suite reservation")
    deadline = time.monotonic() + args.wait_seconds
    while (holders["build"] or holders["suite"] != expected_suite or
           os.getloadavg()[0] > max(1, (os.cpu_count() or 1) * .25)):
        assert time.monotonic() < deadline, "no idle reservation; no measurement started"
        time.sleep(1)
        holders = window.status()
    with ExitStack() as reservations:
        for _ in range(window.limit("build")):
            reservations.enter_context(window.acquire("build", sys.argv, measurement_slot=args.slot))
        if not args.parent_suite:
            for _ in range(window.limit("suite")):
                reservations.enter_context(window.acquire("suite", sys.argv, measurement_slot=args.slot))
        print(f"reserved idle host for {args.slot}; capacities unchanged", flush=True)
        yield


DECLARATIONS = '''int gti_cost_iterations() { return COUNT; }
void gti_cost_begin() {}
int gti_cost_end(c_ulonglong checksum, c_ulonglong effects) {
  if (checksum != c_ulonglong(CHECKSUM) || effects != c_ulonglong(EFFECTS)) { return 1; }
  return 0;
}
'''

CASES = {
    "arithmetic": (0, '''int main() {
  int count = gti_cost_iterations(); mut uint64_t value = 7;
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) { value = (value * 33 + uint64_t(i)) % 1000003; }
  return gti_cost_end(c_ulonglong(value), c_ulonglong(0));
}'''),
    "branches": (1, '''int main() {
  int count = gti_cost_iterations(); mut uint64_t value = 7;
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    if (value % 2 == 0) { value = (value * 33 + uint64_t(i)) % 1000003; }
    else { value = (value * 17 + uint64_t(i)) % 1000003; }
  }
  return gti_cost_end(c_ulonglong(value), c_ulonglong(0));
}'''),
    "arrays": (2, '''int main() {
  int count = gti_cost_iterations(); mut uint64_t values[7] = {1,2,3,4,5,6,7};
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    int index = i % 7; values[index] = (values[index] * 3 + uint64_t(i)) % 1009;
  }
  mut uint64_t sum = 0;
  for (mut int i = 0; i < 7; i += 1) { sum += values[i]; }
  return gti_cost_end(c_ulonglong(sum), c_ulonglong(0));
}'''),
    "text": (3, '''int main() {
  int count = gti_cost_iterations(); std::string_view text = "abcdefghijklmnop";
  mut uint64_t value = 7;
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    value = (value * 33 + uint64_t(uint8_t(text[std::size_t(i % 16)]))) % 1000003;
  }
  return gti_cost_end(c_ulonglong(value), c_ulonglong(0));
}'''),
    "classes": (4, '''mut int dropped = 0;
class Counter {
  mut uint64_t value = 7;
public:
  Counter() {}
  ~Counter() { dropped += 1; }
  void step(int input) mut { this.value = (this.value * 33 + uint64_t(input)) % 1000003; }
  uint64_t read() { return this.value; }
};
int main() {
  int count = gti_cost_iterations(); mut uint64_t result = 0;
  gti_cost_begin();
  { mut Counter value{};
    for (mut int i = 0; i < count; i += 1) { value.step(i % 31); }
    result = value.read();
  }
  return gti_cost_end(c_ulonglong(result), c_ulonglong(dropped));
}'''),
    "tls": (5, '''thread_local mut uint64_t value = 7;
int main() {
  int count = gti_cost_iterations(); gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) { value = (value * 33 + uint64_t(i)) % 1000003; }
  return gti_cost_end(c_ulonglong(value), c_ulonglong(0));
}'''),
    "cleanup": (6, '''mut uint64_t observed = 7;
mut uint64_t dropped = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { observed = (observed * 33 + uint64_t(this.id)) % 1000003; dropped += 1; }
};
int main() {
  int count = gti_cost_iterations(); gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) { Ticket value{i % 31}; }
  return gti_cost_end(c_ulonglong(observed), c_ulonglong(dropped));
}'''),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--case", choices=tuple(CASES), action="append")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--slot")
    parser.add_argument("--parent-suite", action="store_true")
    parser.add_argument("--wait-seconds", type=int, default=1200)
    parser.add_argument("--iterations", type=int, default=20000000)
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    cache = args.gti_build_cache.read_text()
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$", cache, re.M)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    assert not args.measure or args.slot, "timing requires a coordinated reserved slot"
    assert 1 <= args.iterations <= 1000000000 and args.samples >= 3
    matrix = json.loads((root / "tests/llvm_runtime_cost_matrix.json").read_text())
    assert set(matrix["admitted"]) == set(CASES)
    selected = args.case or list(CASES)
    output.mkdir(parents=True)
    manifest = {"schema": 1, "status": "running", "pending": matrix["pending"],
                "unmeasured_obligations": matrix["unmeasured_obligations"],
                "slot": args.slot, "commands": [], "correctness": [], "timing": [],
                "measurement_contract": "process wall time includes startup, GTI initialization/cleanup and exit; no foreign timer hook",
                "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest()}
    env = dict(os.environ, NO_COLOR="1", GTI_COLOR="never")

    def run(argv, extra_env=None):
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.perf_counter()
        result = subprocess.run([str(x) for x in argv], cwd=root, env=extra_env or env,
                                text=True, capture_output=True, timeout=240)
        result.wall_seconds = time.perf_counter() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        result.cpu_seconds = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
        name = f"command-{len(manifest['commands']):03}.log"
        (output / name).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv], "exit": result.returncode, "log": name,
                                     "wall_seconds": result.wall_seconds, "cpu_seconds": result.cpu_seconds})
        assert result.returncode == 0, str(output / name)
        return result

    def quiet():
        deadline = time.monotonic() + args.wait_seconds
        while os.getloadavg()[0] > max(1, (os.cpu_count() or 1) * .25):
            assert time.monotonic() < deadline, "measurement host did not become quiet"
            time.sleep(1)

    reservation = ExitStack()
    try:
        reservation.enter_context(measurement_window(root, args))
        manifest["head"] = run(["git", "rev-parse", "HEAD"]).stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"]).stdout
        manifest["gti_version"] = run([args.gti, "--version"]).stdout.strip()
        clang = shutil.which(args.clang)
        assert clang
        host = output / "scalar-oracle"
        run([clang, "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O2", "-c",
             root / "tests/fixtures/llvm_optimization/runtime_cost_oracle.c", "-o", output / "oracle.o"])
        run([clang, output / "oracle.o", "-o", host])
        binaries = {}
        for name in selected:
            family, source = CASES[name]
            iterations = args.iterations if args.measure else 10000
            expected = json.loads(run([host, str(family), str(iterations)]).stdout)
            path = output / f"{name}.gti"
            declarations = DECLARATIONS.replace("COUNT", str(iterations)).replace(
                "CHECKSUM", str(expected["checksum"])).replace("EFFECTS", str(expected["effects"]))
            path.write_text(declarations + source + "\n")
            for backend, optimization in [("cpp", "3"), *(('llvm', str(i)) for i in range(4))]:
                binary = output / f"{name}-{backend}-O{optimization}"
                run([args.gti, path, "--backend", backend, f"-O{optimization}", "-o", binary])
                result = run([binary])
                assert not result.stderr and not result.stdout
                manifest["correctness"].append({"case": name, "backend": backend, "optimization": optimization,
                                                 "checksum": expected["checksum"], "effects": expected["effects"]})
                binaries[(name, backend, optimization)] = binary
                size = shutil.which("llvm-size") or shutil.which("size")
                assert size
                run([size, "-A", binary])
        if args.measure:
            quiet()
            manifest["load_before"] = os.getloadavg()
            for name in selected:
                samples = {"cpp": [], "llvm": []}
                for backend in samples:
                    run([binaries[(name, backend, "3")]])
                for index in range(args.samples):
                    for backend in (("cpp", "llvm") if index % 2 == 0 else ("llvm", "cpp")):
                        quiet()
                        result = run([binaries[(name, backend, "3")]])
                        assert not result.stderr and not result.stdout
                        seconds = result.wall_seconds
                        assert seconds >= .1, "sample too short; increase iterations"
                        assert result.cpu_seconds / seconds >= .95, "sample was descheduled"
                        samples[backend].append(seconds)
                manifest["timing"].append({"case": name, "iterations": args.iterations, "seconds": samples,
                                           "median_seconds": {b: statistics.median(s) for b, s in samples.items()}})
            manifest["load_after"] = os.getloadavg()
        manifest["status"] = "passed-selected-admitted-families"
        print(f"PASS: {len(manifest['correctness'])} checked executions; pending families remain explicit")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        reservation.close()
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
