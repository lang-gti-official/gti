#!/usr/bin/env python3
"""Production GTI SIMD acceptance; no passing skip for unwired admission."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument("--output", type=Path)
    outputs.add_argument("--output-root", type=Path)
    parser.add_argument("--profile", choices=("all", "core", "copy-range"), default="all")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    output = (args.output if args.output else args.output_root / uuid.uuid4().hex).resolve()
    assert not output.exists()
    assert args.output_root or not output.is_relative_to(root)
    output.mkdir(parents=True)
    manifest = {"status": "running", "profile": args.profile, "commands": [], "cases": [],
                "remaining_gate": "all profile required; copy-range depends on O-LLVM-08 StorageBoundsCheck",
                "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest()}
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"), GTI_COLOR="never")

    def run(command, *, expected=0):
        result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                text=True, capture_output=True, timeout=240)
        name = f"command-{len(manifest['commands']):03}.log"
        (output / name).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in command], "exit": result.returncode, "log": name})
        assert result.returncode == expected, str(output / name)
        return result

    try:
        source = root / "tests/fixtures/llvm_optimization/simd_lanes.gti"
        modes = [("cpp", 3, standard) for standard in ("c++20", "c++23")]
        modes += [("llvm", level, None) for level in range(4)]
        for backend, level, standard in (modes if args.profile != "copy-range" else []):
            binary = output / f"simd-{backend}-O{level}-{standard or 'native'}"
            arguments = ["--std", standard] if standard else []
            run([args.gti, source, "--backend", backend, f"-O{level}", *arguments, "-o", binary])
            result = run([binary])
            assert not result.stdout and not result.stderr
            manifest["cases"].append({"backend": backend, "optimization": level, "standard": standard})
        failures = (
            ("lane", "std::simd<float, 7> value{}; return int(value[std::size_t(7)]);"),
            ("mask", "std::simd_mask<7> value{}; return value[std::size_t(7)] ? 1 : 0;"),
            ("load", "mut std::simd<float, 4> value{}; std::array<float, 6> data{}; value.copy_from<6>(data, std::size_t(3)); return 0;"),
            ("store", "std::simd<double, 4> value{}; mut std::array<double, 6> data{}; value.copy_to<6>(data, std::size_t(3)); return 0;"),
            ("offset", "mut std::simd<float, 4> value{}; std::array<float, 6> data{}; value.copy_from<6>(data, std::size_t(18446744073709551615)); return 0;"),
        )
        if args.profile == "core":
            failures = failures[:2]
        elif args.profile == "copy-range":
            failures = failures[2:]
        for name, body in failures:
            source = output / f"{name}.gti"
            source.write_text("#include <std/simd>\n#include <std/array>\nint main() { " + body + " }\n")
            expected_failure = None
            for backend, level, standard in modes:
                binary = output / f"{name}-{backend}-O{level}-{standard or 'native'}"
                arguments = ["--std", standard] if standard else []
                run([args.gti, source, "--backend", backend, f"-O{level}", *arguments, "-o", binary])
                result = run([binary], expected=70)
                assert not result.stdout and "index_out_of_bounds" in result.stderr
                assert "<std/simd>:" in result.stderr, "failure lost its lexical library source"
                observed = (result.returncode, result.stdout, result.stderr)
                if expected_failure is None:
                    expected_failure = observed
                assert observed == expected_failure, "failure category, location or propagation differs from the supported comparator"
                manifest["cases"].append({"case": name, "backend": backend, "optimization": level,
                                           "standard": standard, "exit": result.returncode})
        declarations = (
            "std::simd<int, 4> value{};",
            "std::simd<bool, 4> value{};",
            "std::simd<float, 0> value{};",
            "std::simd<float, 65> value{};",
            "std::simd_mask<0> value{};",
            "std::simd_mask<65> value{};",
            "std::simd<float, 4> a{}; std::simd<float, 3> b{}; auto invalid = a + b;",
            "std::simd_mask<4> mask{}; if (mask) { return 1; }",
            "float input[4] = {}; mut float destination[4] = {}; gti_internal::simd_add(input, input, destination);",
        )
        for index, body in enumerate(declarations if args.profile != "copy-range" else ()):
            source = output / f"reject-{index}.gti"
            source.write_text("#include <std/simd>\nint main() { " + body + " return 0; }\n")
            binary = output / f"reject-{index}"
            marker = b"existing-destination-must-survive"
            binary.write_bytes(marker)
            command = [args.gti, source, "--backend", "llvm", "-O3", "-o", binary]
            result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                    text=True, capture_output=True, timeout=240)
            name = f"command-{len(manifest['commands']):03}.log"
            (output / name).write_text(result.stdout + result.stderr)
            manifest["commands"].append({"argv": [str(x) for x in command], "exit": result.returncode, "log": name})
            assert result.returncode != 0 and "GTI-S" in result.stderr, str(output / name)
            assert "GTI-P" not in result.stderr and "GTI-B0003" not in result.stderr, str(output / name)
            assert binary.read_bytes() == marker, "rejection replaced the existing destination"
            manifest["cases"].append({"case": "semantic-rejection", "index": index, "preserved_destination": True})
        assert len(manifest["cases"]) == {"all": 45, "core": 27, "copy-range": 18}[args.profile]
        manifest["status"] = "passed" if args.profile == "all" else f"passed-{args.profile}-only"
        print(f"PASS: {len(manifest['cases'])} SIMD {args.profile} checks; full production gate remains explicit; evidence {output}")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
