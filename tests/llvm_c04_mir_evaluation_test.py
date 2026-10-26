#!/usr/bin/env python3
"""Opt-in MIR-derived transport experiment, never a production acceptance gate."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, directory, name, *, expected=0, timeout=180):
    result = subprocess.run([str(arg) for arg in command], capture_output=True,
                            text=True, check=False, timeout=timeout,
                            env={**os.environ, "NO_COLOR": "1"})
    (directory / f"{name}.stdout").write_text(result.stdout)
    (directory / f"{name}.stderr").write_text(result.stderr)
    (directory / f"{name}.json").write_text(json.dumps({
        "command": [str(arg) for arg in command], "returncode": result.returncode,
    }, indent=2))
    if result.returncode != expected:
        raise AssertionError(f"{name}: exit {result.returncode}, expected {expected}: {result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--status-emitter", required=True, type=Path)
    parser.add_argument("--unwind-emitter", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitized", action="store_true")
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = root / "tests/fixtures/llvm_c04_mir"
    expected_error = "scalar.gti:1: runtime error[GTI-R0001]: integer_overflow in addition\n"
    snapshots = {}
    receipts = []
    for mode, emitter in (("status", args.status_emitter.resolve()),
                          ("unwind", args.unwind_emitter.resolve())):
        for level in ("O0", "O3"):
            directory = output / f"{mode}-{level}"
            directory.mkdir()
            prefix = directory / "generated"
            run([emitter, fixtures / "scalar.gti", prefix, level], directory, "emit")
            snapshot = prefix.with_suffix(".lowered").read_bytes()
            if snapshots and snapshot != next(iter(snapshots.values())):
                raise AssertionError("transport candidates did not consume identical sealed-program text")
            snapshots[(mode, level)] = snapshot
            ir = prefix.with_suffix(".ll").read_text()
            if "noinline" not in ir or "uwtable(sync)" not in ir or "gti.c04.transport" not in ir:
                raise AssertionError("lost explicit experimental boundary/unwind-table metadata")
            if ("__gti_c04_evaluate_raise" in ir) != (mode == "unwind"):
                raise AssertionError("wrong transport implementation in isolated artifact")
            executable = directory / "probe"
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitized else []
            run([args.cxx, "-std=c++20", "-O3", *flags, "-pthread",
                 "-I", root / "runtime/include", "-I", directory,
                 fixtures / "driver.cpp", prefix.with_suffix(".o"),
                 args.runtime.resolve(), "-o", executable], directory, "link")
            for outcome in ("success", "failure"):
                for dispatch in ("direct", "indirect"):
                    result = run([executable, outcome, dispatch], directory,
                                 f"{outcome}-{dispatch}", timeout=15)
                    expected = ("1\n", "") if outcome == "success" else ("", expected_error)
                    if (result.stdout, result.stderr) != expected:
                        raise AssertionError(f"{mode}/{level}/{outcome}/{dispatch}: {result}")
            for negative in ("cleanup", "native_call", "global"):
                destination = directory / negative
                marker = b"existing artifact must survive rejection\n"
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / f"{negative}.gti", destination, level],
                             directory, f"reject-{negative}", expected=65)
                if "C04 evaluation" not in result.stderr:
                    raise AssertionError("negative fixture failed before the structural evaluation boundary")
                if destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("rejected experimental shape overwrote an artifact")
            receipts.append({"mode": mode, "level": level, "emitter_sha256": digest(emitter),
                             "input_sha256": hashlib.sha256(snapshot).hexdigest(),
                             "object_sha256": digest(prefix.with_suffix(".o")),
                             "object_bytes": prefix.with_suffix(".o").stat().st_size,
                             "executable_sha256": digest(executable)})
            print(f"{mode}/{level}: exact success/failure, direct/indirect and 3 rejections passed", flush=True)
    (output / "evidence.json").write_text(json.dumps({
        "scope": "no-cleanup scalar MIR; fixed noinline boundaries; test-owned C++ carrier",
        "production_transport_selected": False,
        "runtime_sha256": digest(args.runtime),
        "sanitized_native_carrier": args.sanitized,
        "llvm_object_instrumented": False,
        "limitations": ["no cleanup, ownership, callbacks, TLS or workers",
                        "no OOM, foreign/forced unwind or artifact-unloading proof",
                        "no timing collected; not a production speedup claim"],
        "receipts": receipts,
    }, indent=2))


if __name__ == "__main__":
    main()
