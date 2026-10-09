#!/usr/bin/env python3
"""Runtime-optimization audit screen: GTI probes against idiomatic C++ twins.

`compare` builds each probe's main.gti with GTI and idiomatic.cpp with the
same native policy GTI uses, requires identical stdout, then times both with
interleaved samples.

`attribute` rebuilds a probe's emitted C++ under measurement-only variants:
a raised native inline threshold, and guard rewrites that make one check
family always succeed. The rewrites are UNSAFE ceilings for attribution, not
optimizations: they are valid only because these inputs never fail, and
every variant's stdout must still match the unmodified build.
"""
import argparse
import json
import pathlib
import statistics
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
STRICT = ["-fno-fast-math", "-ffp-contract=off"]
INLINE = ["-mllvm", "-inline-threshold=2000"]
PROBES = sorted(path.name for path in HERE.iterdir() if (path / "idiomatic.cpp").is_file())

GUARDS = {
    "arith": [("if (overflow) {", "if (false) {")],
    "bounds": [("  if (index >= size) {\n    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1, detail};",
                "  if (false) {\n    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1, detail};")],
    "conv": [("    if (!std::in_range<integer_comparison_type<Target>>(\n"
              "            static_cast<integer_comparison_type<Source>>(value))) {",
              "    if (false) {")],
    "slots": [("    if (!engaged_) {\n"
               "      storage_error(\"verified MIR lifetime slot was read while empty\");",
               "    if (false) {\n"
               "      storage_error(\"verified MIR lifetime slot was read while empty\");")],
}
VARIANTS = [
    ("base", [], []),
    ("inline", [], INLINE),
    ("arith", ["arith"], []),
    ("bounds", ["bounds"], []),
    ("conv", ["conv"], []),
    ("slots", ["slots"], []),
    ("all", ["arith", "bounds", "conv", "slots"], INLINE),
]


def run(executable):
    start = time.perf_counter()
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        raise SystemExit(f"{executable} exited {result.returncode}: {result.stderr}")
    return elapsed, result.stdout


def build(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"build failed: {' '.join(command)}\n{result.stderr[-4000:]}")


def compare(arguments):
    rows = []
    for name in arguments.probes or PROBES:
        out = arguments.output / name
        out.mkdir(parents=True, exist_ok=True)
        gti_exe, cpp_exe = out / "gti", out / "cpp"
        build([str(arguments.gti), str(HERE / name / "main.gti"), f"-O{arguments.level}",
               "--cxx", arguments.cxx, "--std", arguments.std,
               "-o", str(gti_exe), "--progress=never", "--color=never"])
        build([arguments.cxx, f"-O{arguments.level}", f"-std={arguments.std}", *STRICT,
               str(HERE / name / "idiomatic.cpp"), "-o", str(cpp_exe)])
        reference = run(gti_exe)[1]
        if run(cpp_exe)[1] != reference:
            raise SystemExit(f"{name}: GTI and C++ outputs differ")
        gti_samples, cpp_samples = [], []
        for _ in range(arguments.runs):
            gti_samples.append(run(gti_exe)[0])
            cpp_samples.append(run(cpp_exe)[0])
        gti_ms = statistics.median(gti_samples) * 1000
        cpp_ms = statistics.median(cpp_samples) * 1000
        gti_min, cpp_min = min(gti_samples) * 1000, min(cpp_samples) * 1000
        rows.append({"probe": name, "gti_median_ms": gti_ms, "cpp_median_ms": cpp_ms,
                     "gti_min_ms": gti_min, "cpp_min_ms": cpp_min,
                     "median_ratio": gti_ms / cpp_ms, "min_ratio": gti_min / cpp_min,
                     "gti_samples": gti_samples, "cpp_samples": cpp_samples,
                     "output": reference.strip()})
        print(f"{name:13} median gti {gti_ms:8.1f} cpp {cpp_ms:8.1f} ({gti_ms / cpp_ms:5.2f}x)"
              f"  min gti {gti_min:8.1f} cpp {cpp_min:8.1f} ({gti_min / cpp_min:5.2f}x)",
              flush=True)
    (arguments.output / "compare.json").write_text(json.dumps(rows, indent=2) + "\n")


def patched(source, families):
    for family in families:
        for old, new in GUARDS[family]:
            if old not in source:
                raise SystemExit(f"emitted C++ no longer contains the {family} guard")
            source = source.replace(old, new)
    return source


def attribute(arguments):
    native = [arguments.cxx, f"-std={arguments.std}", f"-O{arguments.level}", "-I",
              str(arguments.runtime_include), "-I", str(arguments.vendor_include),
              *STRICT, "-D__gti_strict_ieee754=1"]
    rows = []
    for name in arguments.probes or PROBES:
        out = arguments.output / name
        out.mkdir(parents=True, exist_ok=True)
        emitted = out / "emitted.cpp"
        build([str(arguments.gti), str(HERE / name / "main.gti"), f"-O{arguments.level}",
               "--cxx", arguments.cxx, "--std", arguments.std,
               "--emit-cpp", "-o", str(emitted), "--progress=never", "--color=never"])
        source = emitted.read_text()
        row, reference = {"probe": name}, None
        for label, families, flags in VARIANTS:
            cpp, exe = out / f"{label}.cpp", out / label
            cpp.write_text(patched(source, families))
            build(native + flags + [str(cpp), str(arguments.runtime_library), "-o", str(exe)])
            samples = []
            for _ in range(arguments.runs):
                seconds, stdout = run(exe)
                samples.append(seconds)
                if reference is None:
                    reference = stdout
                elif stdout != reference:
                    raise SystemExit(f"{name}/{label}: output changed")
            # Minimum of the samples: these CPU-bound probes only get slower
            # under interference, so the minimum is the robust estimate.
            row[label] = min(samples) * 1000
        rows.append(row)
        print(f"{name:13} " + "  ".join(f"{label} {row[label]:7.1f}" for label, _, _ in VARIANTS),
              flush=True)
    (arguments.output / "attribute.json").write_text(json.dumps(rows, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["compare", "attribute"])
    parser.add_argument("--gti", type=pathlib.Path, required=True)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--std", default="c++23")
    parser.add_argument("--level", default="2", choices=["1", "2", "3"])
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--runtime-include", type=pathlib.Path,
                        default=REPOSITORY / "runtime" / "include")
    parser.add_argument("--runtime-library", type=pathlib.Path,
                        help="libgti_runtime.a from the same build (attribute only)")
    parser.add_argument("--vendor-include", type=pathlib.Path,
                        default=REPOSITORY / "vendor" / "expected-lite" / "include",
                        help="expected-lite include directory for C++20 attribution")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="new directory for builds and JSON results")
    parser.add_argument("probes", nargs="*", help=f"subset of: {', '.join(PROBES)}")
    arguments = parser.parse_args()
    unknown = sorted(set(arguments.probes) - set(PROBES))
    if unknown:
        raise SystemExit(f"unknown probes: {', '.join(unknown)}")
    if arguments.output.exists() and any(arguments.output.iterdir()):
        raise SystemExit("--output must be a new or empty directory")
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.mode == "attribute":
        if arguments.runtime_library is None:
            raise SystemExit("attribute requires --runtime-library")
        attribute(arguments)
    else:
        compare(arguments)


if __name__ == "__main__":
    sys.exit(main())
