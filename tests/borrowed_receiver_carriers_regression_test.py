#!/usr/bin/env python3
"""Cleanup-bound carriers borrowed through captures, formals and accessors.

Each fixture once failed MIR construction on both backends: a carrier made by
a method of a by-reference capture's referent or of an accessor's result, and
a synchronized guard taken through a shared or mutable formal. The C++ oracle
and native LLVM must both build it and agree on exit status, stdout and
stderr at O0 and O2.
"""

import pathlib
import subprocess
import sys
import tempfile

FIXTURES = {
    "borrowed_carrier_capture_receiver.gti":
        "view released\nview released\ncapture carriers done\n"
        "capture guards done\n",
    "borrowed_carrier_accessor_receiver.gti":
        "view released\naccessor carriers done\n",
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
            for backend in ("cpp", "llvm"):
                for level in ("O0", "O2"):
                    executable = root / f"{source.stem}-{backend}-{level}"
                    built = run([str(compiler), str(source), "--backend", backend,
                                 f"-{level}", "--execution-profile", "concurrent",
                                 "-o", str(executable)])
                    if built.returncode != 0:
                        failures.append(f"build {name} {backend} {level}: "
                                        f"{built.stderr[-1500:]}")
                        continue
                    ran = run([str(executable)])
                    observed = (ran.returncode, ran.stdout, ran.stderr)
                    if observed != (0, stdout, ""):
                        failures.append(f"run {name} {backend} {level}: {observed!r}")
                        continue
                    print(f"{name} {backend} {level}: matched", flush=True)
    if failures:
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
