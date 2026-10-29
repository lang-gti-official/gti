#!/usr/bin/env python3
"""Public panic: terminal MIR cleanup, caller provenance, formatting and tasks."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(command, environment):
    return subprocess.run(command, capture_output=True, text=True,
                          env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--worker-only", action="store_true",
                        help="run the focused emergency-handoff fixtures")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    source = fixtures / "panic_runtime.gti"
    lines = source.read_text().splitlines()
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def site(marker, fixture=source):
        fixture_lines = lines if fixture == source else fixture.read_text().splitlines()
        matches = [number for number, line in enumerate(fixture_lines, 1)
                   if line.rstrip().endswith(f"// {marker}")]
        assert len(matches) == 1, (marker, matches)
        return f"{fixture.name}:{matches[0]}"

    def panic(marker, message, truncated=False, fixture=source):
        return (f'{site(marker, fixture)}: runtime error[GTI-R0017]: panic: "{message}"'
                + (" [truncated]" if truncated else "") + "\n")

    def cleanup(primary, message, fixture=source):
        return (f"{site('cleanup-site', fixture)}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0017] at {site(primary, fixture)} "
                f'message="{message}"; secondary [GTI-R0017] '
                'message="cleanup failed"\n')

    # std::function is source-defined: its untracked concrete model retains
    # the selected FunctionItem identity but establishes a fresh call site.
    functional = fixtures.parent.parent / "stdlib" / "std" / "functional.gti"
    functional_lines = functional.read_text().splitlines()
    model_start = next(index for index, line in enumerate(functional_lines)
                       if line.startswith("class function_model<"))
    model_call = next(index + 1 for index, line in enumerate(functional_lines)
                      if index > model_start and "return this.callable(args...)" in line)
    stored_library_panic = (
        f'<std/functional>:{model_call}: runtime error[GTI-R0017]: panic: "stored 6"\n'
    )

    cases = [
        (0, 0, "ready\n90\n", ""),
        (1, 70, "10\n90\n", panic("raw-site", "raw panic with {literal braces}")),
        (2, 70, "1\n2\n20\n90\n", panic("format-site", "arguments 1 then 2")),
        (3, 70, "30\n90\n", panic("owned-site", "owned message survives cleanup")),
        (4, 70, "40\n90\n", panic("truncation-site", "a" * 512, True)),
        (5, 70, "50\n", cleanup("primary-site", "primary failure")),
        (6, 70, "60\n90\n", panic("invalid-format-site", "Panic message formatting failed: invalid format")),
        (7, 70, "70\n90\n", panic("count-site", "Panic message formatting failed: argument count mismatch")),
        (8, 70, "80\n90\n", panic("unsupported-site", "Panic message formatting failed: unsupported argument")),
        (9, 70, "81\n90\n", panic("worker-site", "worker failed")),
        (10, 70, "81\n90\n", panic("worker-site", "worker failed")),
        (11, 70, "90\n", panic("tracked-site", "tracked wrapper")),
        (12, 70, "90\n", panic("untracked-site", "untracked boundary")),
        (13, 70, "90\n", panic("generic-site", "generic 42")),
        (14, 70, "90\n", panic("value-site", "value function failed")),
        (15, 70, "82\n", cleanup("worker-primary-site", "worker primary failure")),
        (16, 70, "90\n", stored_library_panic),
        (17, 70, "90\n", panic("stored-wrapper-site", "stored 7")),
        (18, 70, "90\n", panic("tracked-stored-site", "stored 8")),
        (19, 70, "84\n90\n", panic("owned-format-site", "owned first second")),
        (20, 70, "85\n90\n", panic("terminal-argument-site", "argument failed")),
        (21, 70, "86\n90\n", panic("terminal-argument-site", "argument failed")),
    ]
    modes = [(f"cpp{standard}-O{level}", f"c++{standard}", f"-O{level}", [])
             for standard in ("20", "23") for level in ("0", "1", "3")]
    modes += [("no-elide", "c++23", "-O0", ["-fno-elide-constructors"]),
              ("sanitized", "c++23", "-O1", ["-fsanitize=address,undefined",
                                                 "-fno-omit-frame-pointer"])]
    if arguments.quick:
        modes = [("quick", "c++23", "-O0", [])]
    with tempfile.TemporaryDirectory(prefix="gti-panic-") as temporary:
        root = pathlib.Path(temporary)
        for name, standard, level, native in modes:
            outcomes = 0
            for fixture, executions in (
                    (fixtures / "panic_direct.gti", [(0, 70, "", 'panic_direct.gti:4: runtime error[GTI-R0017]: panic: "explicit panic"\n')]),
                    (fixtures / "panic_generic_direct.gti", [
                        (0, 70, "", panic("generic-direct-site", "generic tracked panic",
                                          fixture=fixtures / "panic_generic_direct.gti"))]),
                    (fixtures / "panic_exact_carrier.gti", [
                        (0, 70, "", panic("exact-carrier-site", "selected 6",
                                          fixture=fixtures / "panic_exact_carrier.gti"))]),
                    (source, cases),
                    (fixtures / "panic_cleanup_free_arguments.gti", [
                        (0, 0, "3\n6\n1\n90\n", ""),
                        (1, 70, "3\n1\n90\n",
                         panic("argument-site", "later argument failed",
                               fixture=fixtures / "panic_cleanup_free_arguments.gti")),
                        (2, 0, "2\n3\n6\n1\n90\n", ""),
                        (3, 70, "2\n1\n90\n",
                         panic("constructor-site", "constructor failed",
                               fixture=fixtures / "panic_cleanup_free_arguments.gti")),
                        (4, 70, "2\n3\n1\n90\n",
                         panic("argument-site", "later argument failed",
                               fixture=fixtures / "panic_cleanup_free_arguments.gti"))]),
                    (fixtures / "panic_task_cleanup.gti", [
                        (0, 70, "worker body\n82\n",
                         cleanup("capture-primary-site", "capture primary failure",
                                 fixtures / "panic_task_cleanup.gti"))]),
                    (fixtures / "panic_worker_construction.gti", [
                        (0, 70, "82\n83\n95\n84\n90\n",
                         panic("construction-primary-site", "allocation primary failure",
                               fixture=fixtures / "panic_worker_construction.gti")),
                        (1, 70, "82\n",
                         cleanup("construction-primary-site", "allocation primary failure",
                                 fixtures / "panic_worker_construction.gti"))]),
                    (fixtures / "panic_worker_shells.gti", [
                        (0, 70, "82\n83\n94\n93\n84\n90\n",
                         panic("shell-primary-site", "shell primary failure",
                               fixture=fixtures / "panic_worker_shells.gti")),
                        (1, 70, "82\n",
                         cleanup("shell-primary-site", "shell primary failure",
                                 fixtures / "panic_worker_shells.gti"))]),
                    (fixtures / "panic_worker_carriers.gti", [
                        (0, 70, "82\n83\n92\n91\n89\n88\n84\n90\n",
                         panic("worker-primary-site", "worker primary failure",
                               fixture=fixtures / "panic_worker_carriers.gti")),
                        (1, 70, "82\n",
                         cleanup("worker-primary-site", "worker primary failure",
                                 fixtures / "panic_worker_carriers.gti"))]),
                    (fixtures / "panic_worker_fresh_carriers.gti", [
                        (0, 70, "82\n83\n92\n91\n89\n88\n84\n90\n",
                         panic("worker-primary-site", "worker primary failure",
                               fixture=fixtures / "panic_worker_fresh_carriers.gti")),
                        (1, 70, "82\n",
                         cleanup("worker-primary-site", "worker primary failure",
                                 fixtures / "panic_worker_fresh_carriers.gti"))])):
                worker_fixture = fixture.name in {
                    "panic_task_cleanup.gti", "panic_worker_construction.gti",
                    "panic_worker_shells.gti", "panic_worker_carriers.gti",
                    "panic_worker_fresh_carriers.gti"}
                if arguments.worker_only and not worker_fixture:
                    continue
                binary = root / f"{fixture.stem}-{name}"
                command = [str(compiler), str(fixture), "--std", standard, level,
                           "--keep-cpp", "-o", str(binary)]
                if fixture == source or worker_fixture:
                    command += ["--execution-profile", "concurrent"]
                if native:
                    command += ["--", *native]
                source_owner = fixture.name == "panic_worker_shells.gti"
                if source_owner:
                    binary.write_bytes(b"preserved")
                    rejected = run(command, environment)
                    assert (rejected.returncode == 65 and "GTI-B0003" in rejected.stderr
                            and "wrapping atomic read-modify-write" in rejected.stderr
                            and binary.read_bytes() == b"preserved"), rejected
                    print(f"RECORDED C++ LIMITATION {fixture.name}/{name}: wrapping atomics", flush=True)
                    command = [str(compiler), str(fixture), "--backend", "llvm", level,
                               "--execution-profile", "concurrent", "-o", str(binary)]
                built = run(command, environment)
                if built.returncode:
                    sys.stderr.write(f"Build failed: {command!r}\n{built.stdout}{built.stderr}")
                    generated = binary.with_suffix(".gti.cpp")
                    if generated.is_file():
                        retained = pathlib.Path(tempfile.mkdtemp(prefix="gti-panic-failure-"))
                        shutil.copy2(generated, retained / generated.name)
                        shutil.copy2(fixture, retained / fixture.name)
                        sys.stderr.write(f"Failure artifacts retained at {retained}\n")
                    return 1
                if not source_owner:
                    generated = binary.with_suffix(".gti.cpp").read_text()
                    assert "// GTI verified-MIR body:" in generated
                    assert "gti_rt_failure_capture_panic_v2" in generated
                for count, status, stdout, stderr in executions:
                    executed = run([str(binary), *(["case"] * count)], environment)
                    actual = (executed.returncode, executed.stdout, executed.stderr)
                    expected = (status, stdout, stderr)
                    if actual != expected:
                        sys.stderr.write(f"{name}/{fixture.stem}, case {count}: expected {expected!r}; got {actual!r}\n")
                        retained = pathlib.Path(tempfile.mkdtemp(prefix="gti-panic-failure-"))
                        for artifact in (fixture, binary, binary.with_suffix(".gti.cpp")):
                            if artifact.exists():
                                shutil.copy2(artifact, retained / artifact.name)
                        sys.stderr.write(f"Failure artifacts retained at {retained}\n")
                        return 1
                outcomes += len(executions)
            print(f"panic: {name}, {outcomes} outcomes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
