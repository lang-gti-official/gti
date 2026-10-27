#!/usr/bin/env python3
"""Self-checking matrices for silent LLVM wrong code.

Every generated program verifies its own results against values derived from
GTI semantics (the literal inputs it was generated from), never from the C++
backend, and counts destructions of tracked objects. A program returns
non-zero on any mismatch. Each cell is built on LLVM at O0 and O2 and on C++
at O0, then run under a watchdog. A cell whose LLVM build succeeds but whose
run fails, crashes, hangs or prints something other than `ok` is wrong code;
the C++ column is a second check where that backend compiles the shape.

    tests/coverage/matrices/llvm_wrongcode_matrix.py build/gti \
        [--stdlib stdlib] [--matrix transfer ...] [--jobs 4] [--out DIR]

Matrices (axes):
  transfer   owned value kind x transfer x class shape
  containers owned element kind x container x operation
  flow       owned element kind x control-flow ownership pattern
  hierarchy  polymorphic holder of owned fields x operation
  bases      concrete-base hierarchy with owned fields x operation
  enums      payload enum shape x operation
  callables  erased callable holding owned state x operation
  order      class shape x end of lifetime, checking destruction order
  dispatch   hierarchy x helper x owner, checking virtual calls during
             construction and destruction (no C++ oracle exists)
  library    standard-library containers holding owned values
"""
import argparse
import concurrent.futures
import os
import re
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from llvm_wrongcode import (bases, callables, containers,  # noqa: E402
                            dispatch, enums, flow, hierarchy, library,
                            order, transfer)

MATRICES = {"transfer": transfer, "containers": containers, "flow": flow,
            "hierarchy": hierarchy, "bases": bases, "enums": enums,
            "callables": callables, "order": order, "dispatch": dispatch,
            "library": library}
ENDPOINTS = (("cpp", "O0"), ("llvm", "O0"), ("llvm", "O2"))


def run(command, env, timeout):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               errors="replace", start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
        return process.returncode, out, err
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        out, err = process.communicate()
        return "timeout", out, err


def endpoint(gti, env, source, backend, level):
    executable = source.with_name(f"{source.stem}-{backend}-{level}")
    rc, out, err = run([str(gti), str(source), "--backend", backend,
                        f"-{level}", "--execution-profile", "concurrent",
                        "-o", str(executable)], env, 600)
    if rc != 0 or not executable.exists():
        codes = sorted(set(re.findall(r"error\[(GTI-[A-Z][0-9]+)\]", out + err)))
        semantic = any(code[4] in "SP" for code in codes)
        return ("rejected:" if semantic else "build:") + ",".join(codes)
    rc, out, _ = run([str(executable)], env, 60)
    return "ok" if rc == 0 and out == "ok\n" else f"FAIL(rc={rc},{out.strip()[:40]!r})"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("gti", type=Path)
    parser.add_argument("--stdlib", type=Path,
                        default=Path(__file__).resolve().parents[3] / "stdlib")
    parser.add_argument("--matrix", nargs="*", choices=sorted(MATRICES),
                        default=sorted(MATRICES))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    env = dict(os.environ, GTI_STDLIB_PATH=str(args.stdlib.resolve()),
               NO_COLOR="1")
    work = args.out or Path(tempfile.mkdtemp(prefix="gti-llvm-wrongcode-"))
    sources = []
    for name in args.matrix:
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        for cell, text in MATRICES[name].cells():
            path = directory / f"{cell}.gti"
            path.write_text(text, encoding="utf-8")
            sources.append(path)

    def classify(source):
        return source, [endpoint(args.gti.resolve(), env, source, backend, level)
                        for backend, level in ENDPOINTS]

    wrong = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for source, (cpp, o0, o2) in pool.map(classify, sources):
            if o0.startswith("FAIL") or o2.startswith("FAIL"):
                wrong += 1
            print(f"{source.parent.name}/{source.stem:40} cpp={cpp:24} "
                  f"llvm-O0={o0:24} llvm-O2={o2}", flush=True)
    print(f"{len(sources)} cells, {wrong} with LLVM wrong code; sources in {work}")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
