#!/usr/bin/env python3
"""C04-E1 experiment, NOT production native-unwind admission.

Example (outputs must be outside the repository):
  python3 tests/llvm_failure_transport_test.py --output /tmp/c04-correctness

Two fixed C++ fixtures compile through LLVM IR to separate native executables.
Status models Abi::gti's result/failure destinations and Boolean success return;
unwind uses Clang's platform C++ personality and exception storage as a temporary
carrier for the identical GTI runtime record. Explicit schedules, not C++ RAII,
model selected MIR cleanup edges. No GTI parser, evaluator or ABI switch exists
here. Direct/dynamic fixtures are noinline, separate-TU and non-LTO on purpose;
they do not predict whole-program inlining/devirtualization costs.

The fixture uses real capture/validate/terminal services from runtime/src/failure.cpp.
Synthetic pinned descriptors test forwarding, NOT content hashing or unload.
Worker entry/join and C callback containment are models, NOT generated GTI
worker/embedding adapters. Partial construction and transfers are fixed traces,
NOT general MIR lowering or evidence for unfinished O-LLVM-06.

Optional --gti plus --gti-build-cache adds actual current-backend source probes;
without those, the manifest explicitly records that production evidence is owed.
Timing requires a coordinator-approved idle slot and explicit --measure/--slot.
It excludes terminal IO and worker creation, retains checks and counts cleanup.
Linux --wrap audits exception-storage requests (not underlying malloc calls).
Darwin allocation instrumentation and complete build/RSS costs remain pending.

References (mechanism/precedent, never GTI semantic authority):
https://llvm.org/docs/ExceptionHandling.html
https://doc.rust-lang.org/stable/reference/panic.html
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time


TRACES = {
    "direct-success": ("CBADEedabc", 5),
    "dynamic-success": ("CBADEedabc", 5),
    "direct-failure": ("CBADdabc", 4),
    "dynamic-failure": ("CBADdabc", 4),
    "caller": ("CBADdabc", 4),
    "secondary": ("CBADda", 2),
    "return-cleanup": ("CBADEedarbc", 6),
    "before-transfer": ("PFp", 1),
    "after-transfer": ("PTCBADdabcp", 5),
    "worker": ("CBADdabcWJ", 4),
    "ffi": ("CBADdabcF", 4),
}
PRIMARY = 'origin.gti:41: runtime error[GTI-R0017]: panic: "owned-before-cleanup"\n'


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--base", default="origin/main",
                        help="explicit base SHA for managed snapshots without remote refs")
    parser.add_argument("--optimizations", nargs="+", choices=("0", "1", "2", "3"),
                        default=["0", "3"])
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--gti", type=Path)
    parser.add_argument("--gti-build-cache", type=Path)
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--slot", help="coordinator-approved quiet-host slot identifier")
    parser.add_argument("--iterations", type=int, default=200000)
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests/fixtures/llvm_failure_transport"
    output = args.output.resolve()
    check(not output.is_relative_to(root), "experiment results must be outside the repository")
    check(not output.exists(), "use a fresh output directory (evidence is never overwritten)")
    check(not args.measure or (args.slot and not args.sanitize and "3" in args.optimizations),
          "timing requires --slot, O3, and no sanitizer")
    check(1 <= args.iterations <= 100000000 and args.samples >= 3, "invalid measurement bounds")
    check(platform.system() == "Darwin" and platform.machine() == "arm64" or
          platform.system() == "Linux" and platform.machine() == "x86_64",
          "this experiment is limited to Darwin AArch64 and Linux x86_64 System V")
    clang_path = shutil.which(args.clang)
    check(clang_path, "Clang is required; no substitute C++ frontend")
    # Keep argv[0]'s ++ spelling: resolving the symlink can select C link mode.
    clang = str(Path(clang_path).absolute())
    cc = str(Path(clang).with_name("clang"))
    check(Path(cc).is_file(), "matching Clang C compiler missing")
    output.mkdir(parents=True)
    manifest = {
        "schema": 1, "status": "running", "kind": "exploratory-not-production-coverage",
        "platform": platform.platform(), "machine": platform.machine(),
        "slot": args.slot, "timing": [], "commands": [], "cases": [], "sizes": [],
        "limitations": ["Clang C++ personality/carrier, not a GTI personality",
                        "fixed cleanup schedules, not general MIR translation",
                        "synthetic pinned descriptors; no unload/pinning or hash proof",
                        "no O-LLVM-06-A2/06-B, generated dynamic/worker/embedding evidence",
                        "no OOM, cancellation, forced/foreign unwind, or unsupported targets",
                        "no complete malloc/emergency-pool audit or whole-compiler build-cost comparison",
                        "no whole-program inline/devirtualization cost evidence"],
    }
    env = dict(os.environ, NO_COLOR="1", GTI_COLOR="never")
    env.pop("TRANSPORT_ALTERNATE", None)

    def save():
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def run(command, *, expected=0, timeout=90, run_env=None):
        number = len(manifest["commands"])
        argv = [str(x) for x in command]
        compile_command = argv[0] in (clang, cc)
        if compile_command and Path("/usr/bin/time").is_file():
            argv = ["/usr/bin/time", "-l" if platform.system() == "Darwin" else "-v", *argv]
        start = time.perf_counter()
        result = subprocess.run(argv, cwd=root, env=run_env or env,
                                text=True, capture_output=True, timeout=timeout)
        seconds = time.perf_counter() - start
        log = output / f"command-{number:03}.log"
        log.write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": argv,
                                     "returncode": result.returncode,
                                     "seconds_diagnostic_only": seconds, "log": log.name})
        save()
        check(result.returncode == expected,
              f"exit {result.returncode}, expected {expected}: {command}; see {log}")
        return result, seconds

    try:
        manifest["head"] = run(["git", "rev-parse", "HEAD"])[0].stdout.strip()
        manifest["base"] = run(["git", "rev-parse", "--verify", f"{args.base}^{{commit}}"])[0].stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"])[0].stdout
        files = [Path(__file__), *sorted(fixtures.iterdir()),
                 root / "runtime/src/failure.cpp", root / "runtime/src/thread.cpp",
                 root / "runtime/src/cthread_state.cpp",
                 root / "runtime/src/failure_unicode_15_1.inc",
                 root / "runtime/src/failure_internal.h", root / "runtime/include/gti/runtime_failure.h"]
        manifest["sha256"] = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in files if p.is_file()}
        manifest["clang"] = run([clang, "--version"])[0].stdout
        common = ["-std=c++20", "-pthread", "-fno-lto", "-g",
                  "-I", root / "runtime/include", "-I", fixtures]
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        runtime = []
        for source in ("failure", "thread", "cthread_state"):
            obj = output / f"{source}.o"
            run([clang, *common, *sanitizer, "-O2", "-c", root / f"runtime/src/{source}.cpp", "-o", obj])
            runtime.append(obj)
        host = output / "host.o"
        run([cc, "-std=c17", "-O2", *sanitizer, "-fno-unwind-tables",
             "-fno-asynchronous-unwind-tables", "-c", fixtures / "host.c", "-o", host])
        binaries = {}
        size_tool = shutil.which("llvm-size")
        check(size_tool, "llvm-size is required for code/unwind section evidence")

        def sizes(path):
            result, _ = run([size_tool, "-A", path])
            sections = {}
            for line in result.stdout.splitlines():
                fields = line.split()
                if len(fields) == 3 and fields[1].isdigit() and fields[2].isdigit():
                    sections[fields[0]] = int(fields[1])
            check(sections, "llvm-size produced no section-size evidence")
            manifest["sizes"].append({"file": path.name, "file_bytes": path.stat().st_size,
                                      "section_bytes": sections})
        for optimization in args.optimizations:
            for mode in ("status", "unwind"):
                name = f"{mode}-O{optimization}"
                objects = []
                flags = [*common, *sanitizer, f"-O{optimization}",
                         f"-DTRANSPORT_STATUS={int(mode == 'status')}"]
                if mode == "status":
                    flags += ["-fno-exceptions"]
                for source in ("transport", "driver"):
                    ir = output / f"{name}-{source}.ll"
                    obj = output / f"{name}-{source}.o"
                    run([clang, *flags, "-S", "-emit-llvm", fixtures / f"{source}.cpp", "-o", ir])
                    text = ir.read_text()
                    if source == "transport":
                        if mode == "unwind":
                            for instruction in ("invoke ", "landingpad ", "resume ", "__gxx_personality_v0"):
                                check(instruction in text, f"native EH machinery missing: {instruction}")
                        else:
                            check("landingpad " not in text and "invoke " not in text,
                                  "status fixture unexpectedly unwinds")
                        check(re.search(r"(?:call|invoke)[^\n]* %\w+\(", text),
                              "indirect invocation was optimized away")
                    run([clang, *sanitizer, f"-O{optimization}", "-c", ir, "-o", obj])
                    sizes(obj)
                    objects.append(obj)
                binary = output / name
                run([clang, "-pthread", *sanitizer, *objects, *runtime, host, "-o", binary])
                sizes(binary)
                run((["otool", "-L"] if platform.system() == "Darwin" else ["ldd"]) + [binary])
                binaries[(mode, optimization)] = binary
                for case, (trace, drops) in TRACES.items():
                    success = case.endswith("success")
                    stderr = "" if success else PRIMARY
                    if case == "caller":
                        stderr = PRIMARY.replace("origin.gti:41", "caller.gti:73")
                    if case == "secondary":
                        stderr = ('cleanup.gti:89: runtime error[GTI-R0014]: failure_during_cleanup; '
                                  'primary [GTI-R0017] at origin.gti:41 message="owned-before-cleanup"; '
                                  'secondary [GTI-R0017] message="secondary-owned"\n')
                    result, _ = run([binary, case], expected=0 if success else 70)
                    expected_stdout = (f"trace={trace} drops={drops} published={int(success)} "
                                       f"result={93 if success else -999}\n")
                    check(result.stdout == expected_stdout and result.stderr == stderr,
                          f"{name}/{case}: observable mismatch: {result.stdout!r}, {result.stderr!r}")
                    manifest["cases"].append(f"{name}/{case}")
                result, _ = run([binary, "dynamic-success"],
                                run_env=dict(env, TRANSPORT_ALTERNATE="1"))
                check(result.stdout == "trace=CBADEedabc drops=5 published=1 result=93\n" and
                      not result.stderr, "alternate dynamic target mismatch")

        # A separate Linux audit leaves instrumentation out of timing binaries.
        if platform.system() == "Linux":
            for mode in ("status", "unwind"):
                binary = output / f"{mode}-allocation-audit"
                run([clang, *common, "-O3", f"-DTRANSPORT_STATUS={int(mode == 'status')}",
                     "-DTRANSPORT_ALLOCATION_AUDIT=1", fixtures / "driver.cpp",
                     fixtures / "transport.cpp", fixtures / "allocation_audit.cpp",
                     *runtime, host, "-Wl,--wrap=__cxa_allocate_exception", "-o", binary])
                for case in ("direct-success", "dynamic-success", "direct-failure", "dynamic-failure"):
                    result, _ = run([binary, case, "--bench", "100"])
                    count = 100 if mode == "unwind" and case.endswith("failure") else 0
                    check(result.stdout.endswith(f"exception_storage_requests={count}\n"),
                          "unexpected exception-storage request count")
        else:
            manifest["allocation_audit"] = "pending: Darwin exception-storage/malloc instrumentation"

        if args.gti:
            check(args.gti_build_cache, "GTI evidence requires its optimized CMakeCache.txt")
            cache = args.gti_build_cache.read_text()
            match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$", cache, re.M)
            check(match, "an optimized GTI compiler is mandatory")
            check(args.gti.resolve().parent == args.gti_build_cache.resolve().parent,
                  "GTI executable must belong to the supplied build cache")
            manifest["gti"] = {"version": run([args.gti, "--version"])[0].stdout,
                               "sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest(),
                               "build_mode": match[1]}
            for backend in ("cpp", "llvm"):
                for optimization in ("0", "3"):
                    for case in ("status_success", "status_failure"):
                        binary = output / f"gti-{backend}-O{optimization}-{case}"
                        run([args.gti, fixtures / f"{case}.gti", "--backend", backend,
                             f"-O{optimization}", "-o", binary], timeout=240)
                        result, _ = run([binary], expected=70 if case.endswith("failure") else 0)
                        if case.endswith("failure"):
                            check(result.stdout == "1\n2\n" and result.stderr ==
                                  "status_failure.gti:11: runtime error[GTI-R0001]: integer_overflow in addition\n",
                                  "actual GTI cleanup/source attribution mismatch")
                        else:
                            check(not result.stdout and not result.stderr, "GTI success mismatch")
                        manifest["cases"].append(binary.name)
        else:
            manifest["gti"] = "pending: no optimized exact-source GTI compiler supplied"

        if args.measure:
            check(os.getloadavg()[0] <= max(1, os.cpu_count() * 0.25), "host is not quiet")
            manifest["load_before"] = os.getloadavg()
            # Warm both variants, then alternate order per sample, never run in parallel.
            for case in ("direct-success", "dynamic-success", "direct-failure", "dynamic-failure"):
                for mode in ("status", "unwind"):
                    run([binaries[(mode, "3")], case, "--bench", str(args.iterations)])
                samples = {"status": [], "unwind": []}
                observables = {}
                for index in range(args.samples):
                    for mode in (("status", "unwind") if index % 2 == 0 else ("unwind", "status")):
                        check(os.getloadavg()[0] <= max(1, os.cpu_count() * 0.25), "host became busy")
                        result, seconds = run([binaries[(mode, "3")], case, "--bench", str(args.iterations)])
                        check(not result.stderr, "timing execution emitted stderr")
                        observables.setdefault(mode, result.stdout)
                        check(observables[mode] == result.stdout, "unstable timing checksum")
                        samples[mode].append(seconds)
                check(observables["status"] == observables["unwind"], "non-equivalent timing workloads")
                manifest["timing"].append({"case": case, "iterations": args.iterations,
                                           "seconds": samples,
                                           "median_seconds": {m: statistics.median(s) for m, s in samples.items()}})
            manifest["load_after"] = os.getloadavg()
        manifest["status"] = "passed-bounded-experiment"
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        save()
    print(f"PASS: {len(manifest['cases'])} bounded cases; evidence: {output / 'manifest.json'}")
    print("C04 remains open; production/platform/cost obligations are listed in the manifest.")


if __name__ == "__main__":
    main()
