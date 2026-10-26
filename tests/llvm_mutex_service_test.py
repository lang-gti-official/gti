#!/usr/bin/env python3
"""Source-verified mutex emitter/runtime boundary, not public lifecycle admission."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cc", default="clang")
    parser.add_argument("--runtime-library", type=Path)
    parser.add_argument("--sanitize", choices=("address,undefined", "thread"))
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--gti", type=Path,
                        help="also run the existing C++ source-cleanup differential")
    args = parser.parse_args()
    if args.gti and args.sanitize:
        parser.error("run the C++ differential separately from native instrumentation")
    root, output = args.root.resolve(), args.output.resolve()
    runtime_library = args.runtime_library or args.emitter.resolve().parent / "libgti_runtime.a"
    output.mkdir(parents=True, exist_ok=True)
    evidence = args.evidence_dir.resolve() if args.evidence_dir else output
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    environment = dict(os.environ)
    if args.sanitize == "address,undefined":
        leaks = int(sys.platform != "darwin")
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        print(f"Address instrumentation and C/C++ UBSan; leak detection={leaks}", flush=True)
    elif args.sanitize == "thread":
        environment["TSAN_OPTIONS"] = "halt_on_error=1"

    def run(name, argv, death=False, expected=0, failure_mode=None):
        start = time.monotonic()
        execution_environment = dict(environment)
        execution_environment.pop("GTI_O13_PROCESS_FAILURE", None)
        if failure_mode:
            execution_environment["GTI_O13_PROCESS_FAILURE"] = failure_mode
        result = subprocess.run([str(value) for value in argv], capture_output=True,
                                text=True, timeout=60, env=execution_environment)
        (evidence / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(value) for value in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - start})
        (evidence / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        valid = result.returncode in (-signal.SIGILL, -signal.SIGTRAP) if death else result.returncode == expected
        if not valid:
            raise AssertionError(f"{name}: see {evidence / (name + '.log')}")
        print(f"{name}: PASS", flush=True)
        return result.stdout

    sanitize = [args.sanitize] if args.sanitize else []
    run("emit", [args.emitter.resolve(), root, output / "mutex.ll", *sanitize])
    flags = ["-O3", "-pthread", "-Wall", "-Wextra", "-Werror", "-I", root / "runtime/include"]
    instrumentation = [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    flags += instrumentation
    run("runtime", [args.cxx, "-std=c++20", *flags,
        "-Dgti_rt_mutex_lock_v1=gti_test_mutex_real_lock_v1", "-c", root / "runtime/src/mutex.cpp",
        "-Dgti_rt_mutex_initialize_v1=gti_test_mutex_real_initialize_v1",
        "-Dgti_rt_mutex_destroy_v1=gti_test_mutex_real_destroy_v1",
        "-o", output / "runtime.o"])
    run("fixture", [args.cc, "-std=c17", *flags, "-c", root / "tests/fixtures/llvm_mutex_service.c",
                    "-o", output / "fixture.o"])
    worker_objects = []
    for name, source in (("quarantine", "tests/fixtures/llvm_native_quarantine.cpp"),
                         ("thread-runtime", "runtime/src/thread.cpp"),
                         ("cthread-runtime", "runtime/src/cthread_state.cpp")):
        target = output / f"{name}.o"
        run(name, [args.cxx, "-std=c++20", *flags, "-c", root / source, "-o", target])
        worker_objects.append(target)
    run("emit-source-composition", [args.emitter.resolve(), root, "--source-composition",
                                     output / "source.ll", *sanitize])
    run("source-observer", [args.cc, "-std=c17", *flags, "-c",
                             root / "tests/fixtures/llvm_native_process_storage.c",
                             "-o", output / "source-observer.o"])
    run("observed-failure-runtime", [args.cxx, "-std=c++20", *flags,
                                      "-Dgti_rt_failure_terminate_v2=gti_test_real_failure_terminate_v2",
                                      "-c", root / "runtime/src/failure.cpp",
                                      "-o", output / "failure-runtime.o"])
    run("emit-active-source", [args.emitter.resolve(), root, "--active-source-composition",
                                output / "active-source.ll", *sanitize])
    run("active-source-observer", [args.cc, "-std=c17", *flags, "-c",
                                    root / "tests/fixtures/llvm_mutex_source_lifecycle.c",
                                    "-o", output / "active-source-observer.o"])
    run("active-failure-runtime", [args.cxx, "-std=c++20", *flags,
                                    "-Dgti_rt_failure_terminate_v2=gti_test_real_failure_terminate_v2",
                                    "-Dgti_rt_failure_terminate_cleanup_v2=gti_test_real_failure_terminate_cleanup_v2",
                                    "-c", root / "runtime/src/failure.cpp",
                                    "-o", output / "active-failure-runtime.o"])
    for mode in ("O0", "O1", "O2", "O3"):
        run(f"ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation, "-c", output / "mutex.ll",
                           "-o", output / f"mutex-{mode}.o"])
        run(f"active-ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation, "-c",
                                  output / "mutex-active.ll", "-o", output / f"mutex-active-{mode}.o"])
        run(f"process-ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation, "-c",
                                   output / "mutex-process.ll", "-o", output / f"mutex-process-{mode}.o"])
        if args.sanitize:
            marker = "__tsan_read" if args.sanitize == "thread" else "__asan_report_"
            for family in ("mutex", "mutex-active", "mutex-process"):
                symbols = run(f"{family}-instrumentation-{mode}",
                              ["nm", "-u", output / f"{family}-{mode}.o"])
                # Fixed internal process roots need global redzones, not a
                # surviving bounds check for statically proved field accesses.
                global_family = "image" if sys.platform == "darwin" else "elf"
                instrumented = (f"__asan_register_{global_family}_globals" in symbols and
                                f"__asan_unregister_{global_family}_globals" in symbols
                                if family == "mutex-process" and
                                args.sanitize == "address,undefined"
                                else marker in symbols)
                if not instrumented:
                    raise AssertionError(
                        f"generated {mode} {family} access lacks {args.sanitize} instrumentation")
        program = output / f"mutex-{mode}"
        run(f"link-{mode}", [args.cxx, "-pthread", *instrumentation, output / "fixture.o", output / "runtime.o",
                             output / f"mutex-{mode}.o", output / f"mutex-active-{mode}.o",
                             output / f"mutex-process-{mode}.o",
                             *worker_objects,
                             runtime_library, "-o", program])
        run(f"execute-{mode}", [program])
        run(f"source-ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation,
                                   "-c", output / "source.ll", "-o", output / f"source-{mode}.o"])
        source_program = output / f"source-{mode}"
        run(f"source-link-{mode}", [args.cxx, "-pthread", *instrumentation,
                                     output / "source-observer.o", output / "runtime.o",
                                     output / "failure-runtime.o",
                                     *worker_objects,
                                     output / f"source-{mode}.o", runtime_library,
                                     "-o", source_program])
        run(f"source-execute-{mode}", [source_program])
        for failure_mode in ("before", "partial-first", "partial-last", "after", "entry", "quarantine"):
            run(f"source-{failure_mode}-{mode}", [source_program], expected=70,
                failure_mode=failure_mode)
        run(f"active-source-ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation,
                                          "-c", output / "active-source.ll",
                                          "-o", output / f"active-source-{mode}.o"])
        active_program = output / f"active-source-{mode}"
        run(f"active-source-link-{mode}", [args.cxx, "-pthread", *instrumentation,
                                            output / "active-source-observer.o", output / "runtime.o",
                                            output / "active-failure-runtime.o",
                                            *worker_objects,
                                            output / f"active-source-{mode}.o", runtime_library,
                                            "-o", active_program])
        run(f"active-source-execute-{mode}", [active_program])
        for failure_mode in ("nested", "cell-last", "cell-first", "double"):
            run(f"active-source-{failure_mode}-{mode}", [active_program], expected=70,
                failure_mode=failure_mode)
        for fault in ("active-lock", "wrong-owner", "inactive-read", "retired-lock", "quarantined-unload"):
            run(f"{fault}-{mode}", [program, fault], death=True)

    if args.gti:
        run("cpp-source-observer", [args.cc, "-std=c17", *flags, "-DGTI_CPP_ORACLE", "-c",
                                     root / "tests/fixtures/llvm_mutex_source_lifecycle.c",
                                     "-o", output / "cpp-source-observer.o"])
        for standard, mode in (("c++20", "O0"), ("c++23", "O3")):
            program = output / f"cpp-active-{standard}"
            run(f"cpp-source-build-{standard}", [args.gti.resolve(), "--execution-profile", "concurrent",
                                                  "--std", standard, f"-{mode}",
                                                  root / "tests/fixtures/llvm_mutex_source_lifecycle.gti",
                                                  "-o", program, "--", output / "cpp-source-observer.o",
                                                  output / "runtime.o", output / "active-failure-runtime.o"])
            run(f"cpp-source-execute-{standard}", [program])
            for failure_mode in ("nested", "cell-last", "cell-first", "double"):
                run(f"cpp-source-{failure_mode}-{standard}", [program], expected=70,
                    failure_mode=failure_mode)


if __name__ == "__main__":
    main()
