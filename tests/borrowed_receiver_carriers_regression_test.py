#!/usr/bin/env python3
"""Cleanup-bound carriers borrowed through captures, formals and accessors.

Each fixture once failed MIR construction on both backends: a carrier made by
a method of a by-reference capture's referent or of an accessor's result, a
synchronized guard taken through a shared or mutable formal, a closure
holding an owner with a destructor beside a by-reference capture, a local
reference alias of a by-reference capture, and a closure that moves in a
cleanup-bound carrier. Every mode tests/backend_modes.py selects must build
it and produce the expected exit status, stdout and stderr at O0 and O2.
"""

import pathlib
import subprocess
import sys
import tempfile

from backend_modes import (check_unsupported, native_modes, record_symptom,
                           unsupported)

FIXTURES = {
    "borrowed_carrier_capture_receiver.gti":
        "view released\nview released\ncapture carriers done\n"
        "capture guards done\n",
    "borrowed_carrier_accessor_receiver.gti":
        "view released\naccessor carriers done\n",
    "closure_cleanup_capture_borrow.gti":
        "closure still in scope\naudited released\n1\nclosure released\n",
    "capture_reference_aliases.gti":
        "read 48\nbump 11\nbump 23\ncount 5\ncell 19\nsum 17\nswapped 32\n"
        "nested 16\n",
    "closure_bound_carrier_capture.gti":
        "closure still in scope\nwatcher 1 saw 4\nwatcher 2 saw 5\n"
        "watcher 3 saw 6\nafter consume\ntotal 157\n",
}

# The frozen C++ backend rejects a formatting call's scalar temporary
# reference arguments; LLVM keeps this fixture's execution evidence.
CPP_UNSUPPORTED = {
    "capture_reference_aliases.gti":
        "scalar, plain-enum, and text temporary reference arguments",
}


def run(command):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=240)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: borrowed_receiver_carriers_regression_test.py "
                         "<gti> <fixture-directory>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-borrowed-receivers-") as raw:
        root = pathlib.Path(raw)
        for name, stdout in FIXTURES.items():
            source = fixtures / name
            for label, flags, _native in native_modes(("O0", "O2")):
                executable = root / f"{source.stem}-{label}"
                built = run([str(compiler), str(source), *flags,
                             "--execution-profile", "concurrent",
                             "-o", str(executable)])
                limitation = unsupported(name, flags,
                                         cpp_unsupported=CPP_UNSUPPORTED)
                if limitation is not None:
                    if not check_unsupported(built, limitation, name, label,
                                             flags):
                        record_symptom(limitation, name, label, flags,
                                       built.returncode != 0 and
                                       limitation in built.stderr)
                    continue
                if built.returncode != 0:
                    failures.append(f"build {name} {label}: "
                                    f"{built.stderr[-1500:]}")
                    continue
                ran = run([str(executable)])
                observed = (ran.returncode, ran.stdout, ran.stderr)
                if observed != (0, stdout, ""):
                    failures.append(f"run {name} {label}: {observed!r}")
                    continue
                print(f"{name} {label}: matched", flush=True)
    if failures:
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
