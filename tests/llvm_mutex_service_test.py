#!/usr/bin/env python3
"""Public mutex lifecycle, native identity and isolated runtime boundary oracles."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest


def evidence_manifest(root):
    fixtures = {
        "guard": "mutex_basic.gti",
        "guard-cleanup": "llvm_mutex_guard_lifecycle.gti",
        "array-cleanup": "llvm_mutex_array_lifecycle.gti",
        "process-retirement": "llvm_native_process_storage.gti",
        "payload-cleanup": "llvm_mutex_source_lifecycle.gti",
    }
    return build_manifest(
        "llvm_mutex_service",
        {key: (root / "tests/fixtures" / path).read_text(encoding="utf-8")
         for key, path in fixtures.items()},
        {"mutex.public-lifecycle": tuple(fixtures)},
        {key: f"tests/fixtures/{path}" for key, path in fixtures.items()},
        execution_profiles={"mutex.public-lifecycle": "concurrent"})


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cc", default="clang")
    parser.add_argument("--runtime-library", type=Path)
    parser.add_argument("--public-gti", type=Path,
                        help="require public driver O0-O3 admission and execution")
    parser.add_argument("--rejections-only", action="store_true")
    parser.add_argument("--sanitize", choices=("address,undefined", "thread"))
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--gti", type=Path,
                        help="also run the existing C++ source-cleanup differential")
    args = parser.parse_args()
    if args.rejections_only and not args.public_gti:
        parser.error("--rejections-only requires --public-gti")
    if args.gti and args.sanitize:
        parser.error("run the C++ differential separately from native instrumentation")
    if args.public_gti and args.sanitize:
        parser.error("public driver checks need a separate unsanitized run")
    root, output = args.root.resolve(), args.output.resolve()
    runtime_library = args.runtime_library or args.emitter.resolve().parent / "libgti_runtime.a"
    output.mkdir(parents=True, exist_ok=True)
    evidence = args.evidence_dir.resolve() if args.evidence_dir else output
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    environment = dict(os.environ)
    environment["GTI_STDLIB_DIR"] = str(root / "stdlib")
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

    if args.public_gti:
        rejections = {
            "relock": ("std::mutex<int32_t> value{1}; "
                       "auto first = value.lock(); auto second = value.lock();", "GTI-S2017"),
            "move-locked-owner": ("std::mutex<int32_t> value{1}; auto guard = value.lock(); "
                                  "std::mutex<int32_t> moved = std::move(value);", "GTI-S2017"),
            "temporary-guard-escape": ("std::mutex<int32_t> value{1}; "
                                       "int32_t& escaped = value.lock().get();", "GTI-S2017"),
        }
        for name, (body, diagnostic) in rejections.items():
            source = output / f"reject-{name}.gti"
            source.write_text("#include <std/mutex>\nint main() { " + body +
                              " return 0; }\n", encoding="utf-8")
            for backend in ("cpp", "llvm"):
                artifact = output / f"rejected-{name}-{backend}"
                artifact.write_bytes(b"preserved")
                label = f"reject-{name}-{backend}"
                run(label, [args.public_gti.resolve(), "--backend", backend,
                    "--execution-profile", "concurrent", source, "-o", artifact], expected=65)
                log = (evidence / f"{label}.log").read_text(encoding="utf-8")
                if (f"error[{diagnostic}]" not in log or "GTI-B0001" in log or
                        artifact.read_bytes() != b"preserved"):
                    raise AssertionError(f"{label}: wrong rejection or changed artifact")
        for name, source, profile, diagnostic in (
            ("single-threaded", root / "tests/fixtures/mutex_basic.gti", [], "GTI-S2084"),
            ("source-global-cleanup", output / "reject-source-global.gti",
             ["--execution-profile", "concurrent"], "GTI-S2061"),
        ):
            if name == "source-global-cleanup":
                source.write_text("#include <std/mutex>\n"
                    "class Owned { public: ~Owned() {} }; "
                    "std::mutex<Owned> global{Owned()};\nint main() { return 0; }\n",
                    encoding="utf-8")
            artifact = output / f"rejected-{name}"
            artifact.write_bytes(b"preserved")
            label = f"reject-{name}"
            run(label, [args.public_gti.resolve(), "--backend", "llvm", *profile,
                source, "-o", artifact], expected=65)
            log = (evidence / f"{label}.log").read_text(encoding="utf-8")
            if (f"error[{diagnostic}]" not in log or "GTI-B0001" in log or
                    artifact.read_bytes() != b"preserved"):
                raise AssertionError(f"{label}: wrong rejection or changed artifact")
    if args.rejections_only:
        return

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
    run("emit-guard-source", [args.emitter.resolve(), root, "--guard-source-composition",
                               output / "guard-source.ll", *sanitize])
    run("guard-source-observer", [args.cc, "-std=c17", *flags, "-c",
                                   root / "tests/fixtures/llvm_mutex_guard_lifecycle.c",
                                   "-o", output / "guard-source-observer.o"])
    run("array-source-observer", [args.cc, "-std=c17", *flags, "-c",
                                   root / "tests/fixtures/llvm_mutex_array_lifecycle.c",
                                   "-o", output / "array-source-observer.o"])
    run("guard-runtime", [args.cxx, "-std=c++20", *flags,
        "-Dgti_rt_mutex_lock_v1=gti_test_mutex_real_lock_v1",
        "-Dgti_rt_mutex_unlock_v1=gti_test_mutex_real_unlock_v1",
        "-Dgti_rt_mutex_initialize_v1=gti_test_mutex_real_initialize_v1",
        "-Dgti_rt_mutex_destroy_v1=gti_test_mutex_real_destroy_v1",
        "-c", root / "runtime/src/mutex.cpp", "-o", output / "guard-runtime.o"])
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
        run(f"guard-source-ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}",
            *instrumentation, "-c", output / "guard-source.ll",
            "-o", output / f"guard-source-{mode}.o"])
        guard_program = output / f"guard-source-{mode}"
        run(f"guard-source-link-{mode}", [args.cxx, "-pthread", *instrumentation,
            output / "guard-source-observer.o", output / "guard-runtime.o",
            output / "failure-runtime.o", output / f"guard-source-{mode}.o",
            runtime_library, "-o", guard_program])
        run(f"guard-source-execute-{mode}", [guard_program])
        for fault in ("guard-lock", "guard-body"):
            run(f"guard-source-{fault}-{mode}", [guard_program],
                expected=70, failure_mode=fault)
        for fault in ("active-lock", "wrong-owner", "inactive-read", "retired-lock", "quarantined-unload"):
            run(f"{fault}-{mode}", [program, fault], death=True)

        if args.public_gti:
            for family, fixture, observer, runtime, failure, faults in (
                ("process", "llvm_native_process_storage.gti", "source-observer.o",
                 "runtime.o", "failure-runtime.o", ("before", "partial-first", "partial-last",
                                       "after", "entry", "quarantine")),
                ("payload", "llvm_mutex_source_lifecycle.gti", "active-source-observer.o",
                 "runtime.o", "active-failure-runtime.o", ("nested", "cell-last", "cell-first", "double")),
                ("guard", "llvm_mutex_guard_lifecycle.gti", "guard-source-observer.o",
                 "guard-runtime.o", "failure-runtime.o", ("guard-lock", "guard-body")),
            ):
                public_program = output / f"public-{family}-{mode}"
                run(f"public-{family}-build-{mode}", [args.public_gti.resolve(),
                    "--backend", "llvm", "--execution-profile", "concurrent", f"-{mode}",
                    root / "tests/fixtures" / fixture, "-o", public_program,
                    "--", output / observer, output / runtime, output / failure,
                    *worker_objects])
                run(f"public-{family}-execute-{mode}", [public_program])
                for fault in faults:
                    run(f"public-{family}-{fault}-{mode}", [public_program],
                        expected=70, failure_mode=fault)
            guard_program = output / f"public-basic-{mode}"
            run(f"public-basic-build-{mode}", [args.public_gti.resolve(),
                "--backend", "llvm", "--execution-profile", "concurrent", f"-{mode}",
                root / "tests/fixtures/mutex_basic.gti", "-o", guard_program])
            run(f"public-basic-execute-{mode}", [guard_program])
            array_program = output / f"public-array-{mode}"
            run(f"public-array-build-{mode}", [args.public_gti.resolve(),
                "--backend", "llvm", "--execution-profile", "concurrent", f"-{mode}",
                root / "tests/fixtures/llvm_mutex_array_lifecycle.gti", "-o", array_program,
                "--", output / "array-source-observer.o", output / "failure-runtime.o"])
            run(f"public-array-execute-{mode}", [array_program])
            run(f"public-array-bounds-{mode}", [array_program],
                expected=70, failure_mode="array-bounds")

    if args.public_gti:
        for standard in ("c++20", "c++23"):
            program = output / f"cpp-guard-{standard}"
            run(f"cpp-guard-build-{standard}", [args.public_gti.resolve(),
                "--backend", "cpp", "--execution-profile", "concurrent",
                "--std", standard, "-O3", root / "tests/fixtures/mutex_basic.gti",
                "-o", program])
            run(f"cpp-guard-execute-{standard}", [program])

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
