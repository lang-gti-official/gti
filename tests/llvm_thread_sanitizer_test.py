#!/usr/bin/env python3
"""TSan component checks: real GTI plain accesses plus explicit LLVM atomics.

The atomic controls are not admitted GTI atomic lowering or an O13 wait runtime.
The thread mode is private/unwired until Session, cache and runtime composition.
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
                "generated_coverage": "actual GTI plain loads/stores; explicit LLVM atomic controls",
                "pending": ["production Session/mode/cache/runtime linkage", "O13 verified GTI atomic lowering",
                            "O13 wait enrollment/notification/collision/lifetime tests", "both-host composition"],
                "emitter_sha256": hashlib.sha256(args.emitter.read_bytes()).hexdigest()}
    env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1:report_atomic_races=1:ignore_noninstrumented_modules=0:ignore_interceptors_accesses=0")

    def run(command, diagnostic=None):
        result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                text=True, capture_output=True, timeout=180)
        name = f"command-{len(manifest['commands']):03}.log"
        (output / name).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in command], "exit": result.returncode, "log": name})
        if diagnostic:
            assert result.returncode != 0 and diagnostic in result.stderr, str(output / name)
        else:
            assert result.returncode == 0 and "ThreadSanitizer:" not in result.stderr, str(output / name)
        return result

    try:
        clang = shutil.which(args.clang)
        assert clang
        env["PATH"] = str(Path(clang).parent) + os.pathsep + env.get("PATH", "")
        manifest["clang"] = run([clang, "--version"]).stdout
        manifest["head"] = run(["git", "rev-parse", "HEAD"]).stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"]).stdout
        for level in (1, 3):
            for mode in (0, 4):
                obj = output / f"thread-{mode}-O{level}.o"
                run([args.emitter.resolve(), obj, str(mode), str(level), "thread-controls"])
                text = Path(str(obj) + ".ll").read_text()
                generated = re.findall(r"^define [^\n]*@__gti_native_v1_f\d+\([^\n]*\).*?^}", text, re.M | re.S)
                assert generated
                assert any("@__tsan_write4" in body for body in generated) == (mode == 4)
                assert any("@__tsan_read4" in body for body in generated) == (mode == 4)
                for bits in (8, 16, 32, 64):
                    assert bool(re.search(rf"call .*@__tsan_atomic{bits}_store\(", text)) == (mode == 4)
                    assert bool(re.search(rf"call .*@__tsan_atomic{bits}_load\(", text)) == (mode == 4)
                binary = output / f"thread-{mode}-O{level}"
                run([clang, "-std=c++20", "-O1", "-g", "-fsanitize=thread", "-fno-omit-frame-pointer",
                     "-pthread", obj, root / "tests/fixtures/llvm_optimization/thread_sanitizer_consumer.cpp",
                     "-o", binary])
                run([binary, "safe"])
                manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": "safe"})
                if mode == 0:
                    run([binary, "plain-race"])
                    manifest["cases"].append({"mode": mode, "IR_optimization": level,
                                              "case": "uninstrumented-generated-access-negative-control"})
                    continue
                for case in ("plain-race", "atomic-plain-race"):
                    result = run([binary, case], "ThreadSanitizer: data race")
                    manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": case,
                                              "expected_failure_exit": result.returncode})
                for direction in ("generated", "host"):
                    case = f"relaxed-{direction}-publication-32"
                    result = run([binary, case], "ThreadSanitizer: data race")
                    manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": case,
                                              "expected_failure_exit": result.returncode})
                    for bits in (8, 16, 32, 64):
                        case = f"{direction}-publication-{bits}"
                        run([binary, case])
                        manifest["cases"].append({"mode": mode, "IR_optimization": level, "case": case})
        manifest["status"] = "passed-component"
        print(f"PASS: {len(manifest['cases'])} native TSan component controls; production O13 gate remains open")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
