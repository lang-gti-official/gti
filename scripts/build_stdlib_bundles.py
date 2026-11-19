#!/usr/bin/env python3
"""Build target-specific stdlib objects with the staged compiler and runtime."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def build(stage, source):
    stage = stage.resolve()
    data = stage / "share/gti"
    # A library producer's entry must be in its package. Use a private package
    # containing the actual installed sources, with their normal toolchain
    # provenance. Never make the application's package root compiler-trusted.
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-") as directory:
        package = Path(directory)
        shutil.copyfile(source / "cmake/stdlib-bundles/gti.toml", package / "gti.toml")
        shutil.copytree(data / "stdlib", package / "stdlib")
        env = dict(os.environ, GTI_STDLIB_PATH=str(package / "stdlib"),
                   GTI_RUNTIME_LIBRARY=str(stage / "lib/libgti_runtime.a"),
                   GTI_RUNTIME_INCLUDE=str(stage / "include"))
        outputs = []
        for profile in ("dev", "release"):
            command = [str(stage / "bin/gti"), "build", "cmath", "--offline"]
            if profile == "release":
                command.append("--release")
            subprocess.run(command, cwd=package, env=env, check=True)
            bundle, = (package / "build/gti" / profile).glob("*/cmath.gtilib")
            relative = Path("precompiled-stdlib") / bundle.parent.name / profile / bundle.name
            candidate = package / relative
            candidate.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(bundle, candidate)
            outputs.append((candidate, data / relative))

        # Exercise normal named import and re-export from a separate package.
        # Only the scratch copy of the implementation is removed. The source
        # companion still provides constexpr and the ordinary toolchain
        # retains sources for editors and unsupported configurations.
        (package / "stdlib/std/cmath.gti").unlink()
        consumer = package / "consumer"
        shutil.copytree(source / "tests/project/standard_library_bundle", consumer)
        for profile in ([], ["--release"]):
            for command in ("check", "run"):
                subprocess.run([str(stage / "bin/gti"), command, "--offline", *profile],
                               cwd=consumer, env=env, check=True)
        for bundle, destination in outputs:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
                try:
                    with bundle.open("rb") as incoming:
                        shutil.copyfileobj(incoming, temp)
                    temp.close()
                    os.chmod(temp.name, 0o644)
                    os.replace(temp.name, destination)
                finally:
                    Path(temp.name).unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args()
    build(args.stage, Path(__file__).resolve().parents[1])
