#!/usr/bin/env python3
"""Execute the public module example and prove implementation cache invalidation."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

from project_cli_smoke_test import executable_named, run


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    repository = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="gti-module-system-") as temporary:
        for backend in ("llvm", "cpp"):
            app = pathlib.Path(temporary) / backend
            shutil.copytree(repository / "examples/module-system", app)
            command = [compiler, "build", "--release", "--backend", backend,
                       "--verbose"]
            if backend == "cpp":
                # Do not claim C++ parity for newly module-distinct nominal
                # identities. Require a classified limitation, then retain
                # execution comparison for its supported unique-name subset.
                rejected = subprocess.run(command, cwd=app, capture_output=True,
                                          text=True, timeout=120)
                assert rejected.returncode == 65 and "GTI-B0003" in rejected.stderr, rejected.stderr
                assert "same qualified name 'Detail'" in rejected.stderr, rejected.stderr
                assert "GTI-B0001" not in rejected.stderr, rejected.stderr
                api = app / "metrics/src/api.gti"
                api.write_text(api.read_text().replace("Detail", "MetricsDetail"))
                print("C++ boundary: module-distinct nominal homonyms require LLVM")
            start = time.perf_counter()
            run(command, cwd=app)
            cold = time.perf_counter() - start
            binary = executable_named(app / "build/gti/release", "module-system")
            run([str(binary)])
            start = time.perf_counter()
            warm = run(command, cwd=app)
            warm_time = time.perf_counter() - start
            assert "gti: cache hit " in warm.stderr, warm.stderr

            # Changing only a registered body must change the executable.
            body = app / "drawing/src/implementation.gti"
            original = body.read_text()
            body.write_text(original.replace("40 + offset()", "41 + offset()"))
            start = time.perf_counter()
            edited = run(command, cwd=app)
            edit_time = time.perf_counter() - start
            assert "gti: cache hit " not in edited.stderr, edited.stderr
            run([str(binary)], expected=1)
            body.write_text(original)
            run(command, cwd=app)
            run([str(binary)])
            if backend == "cpp":
                previous = binary.read_bytes()
                api = app / "metrics/src/api.gti"
                supported = api.read_text()
                api.write_text(supported.replace("MetricsDetail", "Detail"))
                rejected = subprocess.run(command, cwd=app, capture_output=True,
                                          text=True, timeout=120)
                assert rejected.returncode == 65 and "GTI-B0003" in rejected.stderr, rejected.stderr
                assert binary.read_bytes() == previous, "backend rejection replaced the last valid executable"
                run([str(binary)])
                api.write_text(supported)

            # A reachable private return type does not export its source name.
            main_path = app / "src/main.gti"
            main_path.write_text(main_path.read_text().replace(
                "auto first = drawing::make();", "Detail first = drawing::make();"))
            invalid = subprocess.run(command, cwd=app, capture_output=True,
                                     text=True, timeout=120)
            assert invalid.returncode != 0, "private type name escaped its module"
            assert "Detail" in invalid.stderr, invalid.stderr
            print(f"{backend}: cold={cold:.3f}s warm={warm_time:.3f}s "
                  f"implementation-edit={edit_time:.3f}s bytes={binary.stat().st_size}")


if __name__ == "__main__":
    main()
