#!/usr/bin/env python3
"""Native instrumentation controls over actual GTI-lowered loads and stores.

Component gate until the shared production option/Session registration lands.
The native shim uses the central ABI planner; it is not a production FFI export.
UndefinedMemory means null/alignment checks, not complete Clang UBSan coverage.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--clang", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    output.mkdir(parents=True)
    manifest = {"status": "running", "component_only": True, "commands": [], "cases": [],
                "generated_coverage": "actual GTI load/store ASan; UBSan null/alignment only",
                "pending": ["production option/Session/runtime/cache registration",
                            "whole-GTI undefined-behavior instrumentation",
                            "MIR lifetime endpoints for use-after-scope",
                            "cross-family O08/O09 combined checks"],
                "emitter_sha256": hashlib.sha256(args.emitter.read_bytes()).hexdigest()}
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(command, expected=None):
        result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                text=True, capture_output=True, timeout=180)
        name = f"command-{len(manifest['commands']):03}.log"
        (output / name).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in command], "exit": result.returncode, "log": name})
        if expected is None:
            assert result.returncode == 0, str(output / name)
        else:
            assert result.returncode != 0 and expected in result.stderr, str(output / name)
        return result

    try:
        clang = shutil.which(args.clang)
        assert clang
        manifest["clang"] = run([clang, "--version"]).stdout
        manifest["head"] = run(["git", "rev-parse", "HEAD"]).stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"]).stdout
        for level in (1, 3):
            for mode in range(4):
                obj = output / f"memory-{mode}-O{level}.o"
                run([args.emitter.resolve(), obj, str(mode), str(level)])
                text = Path(str(obj) + ".ll").read_text()
                address = mode in (1, 3)
                undefined = mode in (2, 3)
                assert bool(re.search(r"call void @__asan_report_", text)) == address
                assert bool(re.search(r"call void @__ubsan_handle_type_mismatch_v1_abort", text)) == undefined
                binary = output / f"memory-{mode}-O{level}"
                run([clang, "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                     "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", obj,
                     root / "tests/fixtures/llvm_optimization/sanitizer_consumer.cpp", "-o", binary])
                run([binary, "safe"])
                manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": "safe"})
                if mode == 0:
                    # A sanitized consumer cannot see this uninstrumented GTI read.
                    run([binary, "out-of-bounds"])
                    manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": "consumer-only-negative-control"})
                controls = []
                if address:
                    controls += [("out-of-bounds", "heap-buffer-overflow"), ("use-after-free", "heap-use-after-free")]
                if undefined:
                    controls += [("null", "null pointer"), ("misaligned", "misaligned address")]
                for case, diagnostic in controls:
                    for write in (False, True):
                        selected = case + ("-write" if write else "")
                        result = run([binary, selected], diagnostic)
                        kind = ("WRITE" if write else "READ") if diagnostic.startswith("heap-") else (
                            "store" if write else "load")
                        assert kind in result.stderr
                        manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": selected,
                                                  "diagnostic": diagnostic, "exit": result.returncode})
        manifest["status"] = "passed-component"
        print(f"PASS: {len(manifest['cases'])} real GTI native memory instrumentation controls")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
