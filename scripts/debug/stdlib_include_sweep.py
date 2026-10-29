#!/usr/bin/env python3

"""Compile every standard-library module on its own and report what breaks.

The weakest possible claim about a module is that importing it reaches C++
emission, and the stdlib surface manifest does not exercise this exact shape:
its cases use the module they cover. This sweep protects the import-without-
use path. The script keeps its historical filename for tool compatibility.

    python3 scripts/debug/stdlib_include_sweep.py
    python3 scripts/debug/stdlib_include_sweep.py --gti build/gti

Each module gets a program consisting of one `import` and an empty `main`,
then compiles through `--emit-cpp`, so a failure is attributable to the module
alone without spending time in the native toolchain. Results are:

    ok           the module compiles on its own
    ICE          GTI-B0001 — an internal error from merely importing it
    header-error the module's own source is rejected; the location is inside
                 stdlib/, so a user could not act on it
    error        the import is rejected with a diagnostic pointing at the
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
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent
DIAGNOSTIC = re.compile(r"error\[(GTI-[A-Z0-9]+)\]")


def public_modules(root: Path) -> dict[str, str]:
    """Public names are registered explicitly; private source files are omitted."""
    document = json.loads((root / "stdlib/module-map.json").read_text(encoding="utf-8"))
    default = document.get("default")
    return {name: "std" if name == default else f"std.{name}"
            for name in document["modules"]}


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
    for module in public_modules(root):
        # A case references its module by fixture name or evidence path, so
        # match on the stem appearing anywhere in the manifest rather than
        # guessing the naming convention.
        if module.rsplit(".", 1)[-1] in text:
            mentioned.add(module)
    del document
    return mentioned


def classify(gti: Path, directory: Path, module: str, spelling: str) -> tuple[str, str]:
    source = directory / f"import-{module}.gti"
    source.write_text(f"import {spelling};\n\nint main() {{\n  return 0;\n}}\n",
                      encoding="utf-8")
    run = subprocess.run(
        [str(gti), str(source), "-O0", "--std", "c++23", "--emit-cpp",
         "-o", str(directory / f"{module}.cpp")],
        capture_output=True, text=True, timeout=600)
    if run.returncode == 0:
        return "ok", ""
    output = run.stderr + run.stdout
    codes = DIAGNOSTIC.findall(output)
    first = output.strip().split("\n")[0]
    if "GTI-B0001" in codes:
        detail = first.split("artifact:")[-1].strip() if "artifact:" in first else first
        return "ICE", detail[:140]
    # A diagnostic whose location is inside stdlib/ is the module's own fault,
    # not a requirement the caller failed to meet.
    if "/stdlib/std/" in first:
        return "header-error", first.split("/stdlib/std/")[-1][:140]
    return "error", first[:140]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile each stdlib module alone and report failures.")
    parser.add_argument("--gti", default="build/gti")
    parser.add_argument("--manifest-gaps", action="store_true",
                        help="also list modules with no surface-manifest case")
    arguments = parser.parse_args()

    gti = (REPOSITORY_ROOT / arguments.gti).resolve()
    if not gti.is_file():
        print(f"error: {gti} not found; build it first", file=sys.stderr)
        return 1
    modules = dict(sorted(public_modules(REPOSITORY_ROOT).items()))
    if not modules:
        print("error: no public modules registered in stdlib/module-map.json", file=sys.stderr)
        return 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-import-") as temporary:
        directory = Path(temporary)
        for module, spelling in modules.items():
            state, detail = classify(gti, directory, module, spelling)
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
