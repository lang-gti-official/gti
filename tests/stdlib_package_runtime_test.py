#!/usr/bin/env python3
"""Build and execute the real stdlib test package, including its native source."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from backend_modes import cpp_modes
from sanitizer_support import scaled_timeout


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(repository / "runtime/include"))
    expected = "acc worked\nnumeric tests passed\nfoundation tests passed\n"
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-package-") as directory:
        # The C++ backend runs only when GTI_TEST_BACKENDS selects it.
        for backend in ("llvm", *(("cpp",) if cpp_modes() else ())):
            for profile in ("dev", "release"):
                package = Path(directory) / f"{backend}-{profile}"
                shutil.copytree(repository / "stdlib/tests", package,
                                ignore=shutil.ignore_patterns("build", "gti.lock"))
                command = [str(compiler), "run", "--backend", backend,
                           "--profile", profile]
                result = subprocess.run(command, cwd=package, env=environment,
                                        capture_output=True, text=True,
                                        timeout=scaled_timeout(180))
                assert result.returncode == 0, \
                    f"{command}\n{result.stdout}\n{result.stderr}"
                assert result.stdout == expected, \
                    f"{backend}/{profile}: {result.stdout!r}\n{result.stderr}"
                print(f"stdlib package {backend}/{profile}: passed", flush=True)


if __name__ == "__main__":
    main()
