"""Compare two GTI drivers at -O2. Save the raw measurements outside the source tree."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

SOURCE = Path(__file__).resolve().parent
CASES = [
    ("get", 0, 12000, 0), ("json", 1, 12000, 2), ("fragmented", 2, 5000, 0),
    ("upload4k", 3, 1000, 4096), ("upload1m", 3, 60, 1048576), ("reject", 4, 10000, 0),
]


def build(gti, out, darwin):
    builds = {}
    support = out / "support.o"
    subprocess.run(["cc", "-O2", "-c", SOURCE / "support.c", "-o", support], check=True)
    def binary(name, source, *objects):
        start = time.monotonic()
        with (out / (name + ".log")).open("w") as log:
            subprocess.run([gti, SOURCE / source, "-O2", "-o", out / name, "--", *objects],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        elapsed = time.monotonic() - start
        command = ["size", "-m"] if darwin else ["size", "-A"]
        sections = subprocess.check_output([*command, out / name], text=True)
        pattern = r"Section __text: (\d+)" if darwin else r"(?m)^\.text\s+(\d+)"
        builds[name] = {"seconds": elapsed, "text_bytes": int(re.search(pattern, sections)[1])}
    binary("single", "main.gti", support)
    binary("multi", "two_transports.gti", support)
    if darwin:
        lib = out / "alloc.dylib"
        tracked = out / "tracked.o"
        subprocess.run(["cc", "-O2", "-dynamiclib", SOURCE / "allocations_macos.c", "-o", lib], check=True)
        subprocess.run(["cc", "-O2", "-DHTTP_TRACK_ALLOCATIONS", "-c", SOURCE / "support.c",
                        "-o", tracked], check=True)
        binary("heap", "main.gti", tracked, lib)
        binary("resident", "resident.gti", tracked, lib)
        # A separately compiled C allocation proves the hooks observe both creation and retirement.
        control = out / "control.c"
        control.write_text('#include <stdlib.h>\nextern void http_heap_start(void), http_heap_finish(void);\n'
                           'int main(void) { http_heap_start(); char *p = malloc(123); if (!p) return 1;'
                           ' p[0] = 1; free(p); http_heap_finish(); return 0; }\n')
        subprocess.run(["cc", "-O0", control, lib, "-o", out / "control"], check=True)
        counts = measure(out, "control", 0, 1, 0, 0, heap=True)
        assert counts["allocations"] == 1 and counts["final_live_bytes"] == 0 and counts["peak_live_bytes"] >= 123
    return builds


def measure(out, exe, mode, rounds, size, adapter, heap=False, segmented=False):
    env = dict(os.environ, HTTP_CASE=str(mode), HTTP_ROUNDS=str(rounds),
               HTTP_UPLOAD=str(size), HTTP_ADAPTER=str(adapter))
    if heap:
        env["DYLD_INSERT_LIBRARIES"] = str(out / "alloc.dylib")
    command = [out / exe] + (["segmented"] if segmented else [])
    output = subprocess.check_output(command, env=env, text=True, timeout=60)
    return {name: int(value) for name, value in re.findall(r"(\w+)=(\d+)", output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    darwin = platform.system() == "Darwin"
    report = {"platform": platform.platform(), "builds": build(args.gti.resolve(), out, darwin),
              "cases": {}, "resident": {}, "persistent": {}, "multi": {}}
    for name, mode, rounds, size in CASES:
        samples = [[], []]
        for trial in range(7):
            for driver in [trial % 2, 1 - trial % 2]:
                samples[driver].append(measure(out, "single", mode, rounds, size, driver))
        assert len({r["digest"] for group in samples for r in group}) == 1
        assert len({(r["reads"], r["writes"], r["flushes"]) for group in samples for r in group}) == 1
        for driver, group in enumerate(samples):
            assert all(r["reads"] == group[0]["reads"] and r["writes"] == group[0]["writes"]
                       and r["flushes"] == rounds for r in group)
            row = {"samples": group, "requests_per_second": round(rounds * 1e9 / statistics.median(r["ns"] for r in group)),
                   "p99_ns": statistics.median(r["p99_ns"] for r in group)}
            if darwin:
                alloc = measure(out, "heap", mode, rounds, size, driver, heap=True)
                assert alloc["final_live_bytes"] == 0 and alloc["allocations"] > 0 and alloc["digest"] == group[0]["digest"]
                row["heap"] = alloc
            report["cases"][f"{name}/{driver}"] = row
    for driver in range(2):
        for segmented in [False, True]:
            r = measure(out, "multi", 1, 1000, 2, driver, segmented=segmented)
            assert r["digest"] == report["cases"]["json/0"]["samples"][0]["digest"] // 12
            report["multi"][f"{driver}/{segmented}"] = r
    if darwin:
        for n in (1, 256, 1024):
            for stalled in range(2):
                r = measure(out, "resident", 0, n, 0, stalled, heap=True)
                assert r["final_live_bytes"] == 0 and r["digest"] == report["cases"]["get/0"]["samples"][0]["digest"] // 12000
                report["resident"][f"{n}/{stalled}"] = r
        for n in (1, 100, 20000):
            r = measure(out, "heap", 0, n, 0, 1, heap=True)
            assert r["final_live_bytes"] == 0
            report["persistent"][n] = r
    dest = out / "measurements.json"
    dest.write_text(json.dumps(report, indent=2) + "\n")
    print(dest)


if __name__ == "__main__":
    main()
