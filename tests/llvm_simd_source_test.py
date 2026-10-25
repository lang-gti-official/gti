#!/usr/bin/env python3
"""Production GTI SIMD acceptance; no passing skip for unwired admission."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    output.mkdir(parents=True)
    manifest = {"status": "running", "commands": [], "cases": [],
                "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest()}
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"), GTI_COLOR="never")

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                text=True, capture_output=True, timeout=240)
        name = f"command-{len(manifest['commands']):03}.log"
        (output / name).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in command], "exit": result.returncode, "log": name})
        assert result.returncode == 0, str(output / name)
        return result

    try:
        source = root / "tests/fixtures/llvm_optimization/simd_lanes.gti"
        for backend, level in [("cpp", 3), *(("llvm", i) for i in range(4))]:
            binary = output / f"simd-{backend}-O{level}"
            run([args.gti, source, "--backend", backend, f"-O{level}", "-o", binary])
            result = run([binary])
            assert not result.stdout and not result.stderr
            manifest["cases"].append({"backend": backend, "optimization": level})
        for name, body in (("lane", "std::simd<float, 7> value{}; return int(value[std::size_t(7)]);"),
                           ("mask", "std::simd_mask<7> value{}; return value[std::size_t(7)] ? 1 : 0;")):
            source = output / f"{name}.gti"
            source.write_text("#include <std/simd>\nint main() { " + body + " }\n")
            for level in range(4):
                binary = output / f"{name}-O{level}"
                run([args.gti, source, "--backend", "llvm", f"-O{level}", "-o", binary])
                result = subprocess.run([binary], cwd=root, env=env, text=True, capture_output=True, timeout=15)
                (output / f"{name}-O{level}-failure.log").write_text(result.stdout + result.stderr)
                assert result.returncode == 70 and not result.stdout and "index_out_of_bounds" in result.stderr
                manifest["cases"].append({"case": name, "optimization": level, "exit": result.returncode})
        manifest["status"] = "passed"
        print("PASS: source SIMD arithmetic/masks/reduction/IEEE and checked lane failures")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
