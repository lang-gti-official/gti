#!/usr/bin/env python3

"""Compile every standard-library module on its own and report what breaks.

The weakest possible claim about a header is that including it compiles, and
nothing was checking it. Every case in `tests/stdlib_surface_manifest.json`
*uses* the module it covers, so none exercises the include-without-use path —
which is a real shape with its own failure mode: including a header that
declares a generic, without instantiating it, leaves a destructor declaration
with no concrete MIR instance and raises `GTI-B0001` (see #116). Using the
module fixes it, so the surface cases pass while the header alone does not.

When first run this reported three failures out of nineteen modules, including
`forward_list`, which has two passing manifest cases.

    python3 scripts/debug/stdlib_include_sweep.py
    python3 scripts/debug/stdlib_include_sweep.py --gti build/gti

Each module gets a program consisting of one `#include` and an empty `main`, so
a failure is attributable to the header alone. Results are:

    ok           the header compiles on its own
    ICE          GTI-B0001 — an internal error from merely including it
    header-error the header's own source is rejected; the location is inside
                 stdlib/, so a user could not act on it
    error        the include is rejected with a diagnostic pointing at the
                 user's file, which may be a legitimate requirement

`--manifest-gaps` additionally lists modules the surface manifest never
mentions. Treat it as a weak hint rather than a coverage measure: it matches on
the module stem appearing anywhere in the manifest text, so a substring like
`list` inside `forward_list` counts as a mention.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent
DIAGNOSTIC = re.compile(r"error\[(GTI-[A-Z0-9]+)\]")


def manifest_modules(root: Path) -> set[str]:
    """Module stems that the surface manifest mentions in any case source."""
    manifest = root / "tests" / "stdlib_surface_manifest.json"
    if not manifest.is_file():
        return set()
    text = manifest.read_text(encoding="utf-8")
    try:
        document = json.loads(text)
    except json.JSONDecodeError:
        return set()
    mentioned: set[str] = set()
    for module in (root / "stdlib" / "std").glob("*.gti"):
        # A case references its module by fixture name or evidence path, so
        # match on the stem appearing anywhere in the manifest rather than
        # guessing the naming convention.
        if module.stem in text:
            mentioned.add(module.stem)
    del document
    return mentioned


def classify(gti: Path, cxx: str, directory: Path, module: str) -> tuple[str, str]:
    source = directory / f"include-{module}.gti"
    source.write_text(f"#include <std/{module}>\n\nint main() {{\n  return 0;\n}}\n",
                      encoding="utf-8")
    run = subprocess.run(
        [str(gti), str(source), "-O0", "--std", "c++23",
         "-o", str(directory / module), "--cxx", cxx],
        capture_output=True, text=True, timeout=600)
    if run.returncode == 0:
        return "ok", ""
    output = run.stderr + run.stdout
    codes = DIAGNOSTIC.findall(output)
    first = output.strip().split("\n")[0]
    if "GTI-B0001" in codes:
        detail = first.split("artifact:")[-1].strip() if "artifact:" in first else first
        return "ICE", detail[:140]
    # A diagnostic whose location is inside stdlib/ is the header's own fault,
    # not a requirement the caller failed to meet.
    if "/stdlib/std/" in first:
        return "header-error", first.split("/stdlib/std/")[-1][:140]
    return "error", first[:140]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile each stdlib module alone and report failures.")
    parser.add_argument("--gti", default="build/gti")
    parser.add_argument("--cxx", default=None)
    parser.add_argument("--manifest-gaps", action="store_true",
                        help="also list modules with no surface-manifest case")
    arguments = parser.parse_args()

    gti = (REPOSITORY_ROOT / arguments.gti).resolve()
    if not gti.is_file():
        print(f"error: {gti} not found; build it first", file=sys.stderr)
        return 1
    cxx = arguments.cxx or shutil.which("c++") or "c++"

    modules = sorted(path.stem for path in
                     (REPOSITORY_ROOT / "stdlib" / "std").glob("*.gti"))
    if not modules:
        print("error: no modules found under stdlib/std", file=sys.stderr)
        return 1

    directory = Path(tempfile.mkdtemp(prefix="gti-stdlib-include-"))
    failures = 0
    for module in modules:
        state, detail = classify(gti, cxx, directory, module)
        if state == "ok":
            print(f"  {module:<16} ok")
            continue
        failures += 1
        print(f"  {module:<16} {state:<13} {detail}")

    if arguments.manifest_gaps:
        covered = manifest_modules(REPOSITORY_ROOT)
        gaps = [module for module in modules if module not in covered]
        print()
        if gaps:
            print("no surface-manifest case: " + ", ".join(gaps))
        else:
            print("every module has a surface-manifest case")

    print()
    print(f"{len(modules)} module(s): {len(modules) - failures} ok, "
          f"{failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
