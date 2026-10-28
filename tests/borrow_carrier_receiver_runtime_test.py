#!/usr/bin/env python3
"""Borrows through stored reference fields and returned confined callables run.

A result borrowed from a receiver behind a carrier's stored reference field,
including a mutex guard, and a confined callable returned as its call's exact
result run on both backends. The C++ emitter does not represent a returned
confined callable, so that fixture runs natively only.
"""

import argparse
import os
import pathlib
import subprocess
import tempfile


FIXTURES = pathlib.Path(__file__).parent / "fixtures"
CASES = (
    ("stored_reference_receivers.gti", [], "12\n23\n22\n30\n62\n2\n70\n2\n41\n523\n", True),
    ("synchronized_reference_fields.gti", ["--execution-profile", "concurrent"],
     "3\n56\n2\n1\n1209\n222\n1\n122\n", True),
    ("returned_confined_callables.gti", [], "123\n1231234\n3006\n13\n", False),
    ("returned_owned_closures.gti", [], "29\n2\n", True),
)


def run(command, environment):
    return subprocess.run(command, capture_output=True, text=True, timeout=240,
                          env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    environment = dict(os.environ)
    environment.setdefault(
        "GTI_STDLIB_PATH", str(pathlib.Path(__file__).resolve().parent.parent / "stdlib"))
    modes = [(f"cpp-{level}", ["--std", "c++23", f"-{level}"], True)
             for level in ("O0", "O3")]
    modes += [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"], False)
              for level in ("O0", "O1", "O2", "O3")]
    with tempfile.TemporaryDirectory(prefix="gti-borrow-carrier-") as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, cpp in modes:
            selected = [case for case in CASES if case[3] or not cpp]
            for name, options, expected, _ in selected:
                binary = root / f"{pathlib.Path(name).stem}-{mode}"
                command = [str(compiler), str(FIXTURES / name), *options, *flags,
                           "--progress", "never", "-o", str(binary)]
                built = run(command, environment)
                if built.returncode:
                    raise AssertionError(
                        f"{mode}/{name} build:\n{built.stdout}{built.stderr}")
                executed = run([str(binary)], environment)
                actual = (executed.returncode, executed.stdout, executed.stderr)
                if actual != (0, expected, ""):
                    raise AssertionError(f"{mode}/{name}: {actual!r}")
                binary.unlink()
            print(f"borrow carrier receivers: {mode} passed "
                  f"({len(selected)} fixtures)", flush=True)


if __name__ == "__main__":
    main()
