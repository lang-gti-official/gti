#!/usr/bin/env python3
"""Compile shared/scoped jobs using only a relocated installed toolchain."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(command, environment):
    result = subprocess.run([str(part) for part in command], env=environment,
                            text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result.stdout


def main():
    build = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith(("GTI_", "DYLD_"))}
    with tempfile.TemporaryDirectory(prefix="gti-scoped-installed-") as directory:
        root = Path(directory)
        stage = root / "stage"
        run(["cmake", "--install", build, "--prefix", stage,
             "--component", "gti_toolchain"], environment)
        relocated = root / "relocated toolchain"
        stage.rename(relocated)
        compiler = relocated / "bin" / ("gti.exe" if os.name == "nt" else "gti")
        run([compiler, "--version"], environment)
        for name in ("scoped_parallel_threads", "scoped_parallel_cleanup",
                     "shared_mutex_threads", "transfer_function_threads"):
            source = root / f"{name}.gti"
            shutil.copyfile(fixtures / source.name, source)
            for level in ("O0", "O3"):
                output = root / f"{name}-{level}"
                run([compiler, "--execution-profile", "concurrent", f"-{level}",
                     source, "-o", output], environment)
                assert run([output], environment) == "", (name, level)
    print("Relocated installed scoped borrows and shared/erased jobs passed O0/O3")


if __name__ == "__main__":
    main()
