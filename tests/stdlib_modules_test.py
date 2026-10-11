#!/usr/bin/env python3
"""Every public standard-library unit is importable as exactly one module."""

import os
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, run_cases
from harness_timeouts import scaled


def interface(name):
    return (f"// Module std.{name}: the public surface of <std/{name}>.\n"
            f"export module {name};\n"
            f"export #include <std/{name}>\n")


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    stdlib = pathlib.Path(sys.argv[2]).resolve() / "stdlib"
    units = sorted(path.stem for path in (stdlib / "std").glob("*.gti"))
    modules = sorted(path.stem for path in (stdlib / "modules").glob("*.gti"))
    assert units == modules, (
        "public units and module interfaces differ: "
        f"units only {sorted(set(units) - set(modules))}, "
        f"modules only {sorted(set(modules) - set(units))}")
    for name in modules:
        text = (stdlib / "modules" / f"{name}.gti").read_text(encoding="utf-8")
        assert text == interface(name), f"stdlib/modules/{name}.gti is not canonical"

    environment = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-modules-") as temporary:
        root = pathlib.Path(temporary)

        def check(name):
            source = root / f"{name}.gti"
            source.write_text(f"import std.{name};\nint main() {{ return 0; }}\n",
                              encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "--emit-mir", "-o", os.devnull],
                capture_output=True, text=True, env=environment, timeout=scaled(300))
            assert result.returncode == 0, result.stdout + result.stderr

        run_cases([(name, name) for name in modules], check, configured_jobs())
    print(f"standard-library modules: {len(modules)} interfaces import and lower")


if __name__ == "__main__":
    main()
