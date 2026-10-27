#!/usr/bin/env python3
"""Public concurrent confined callback invocation and terminal boundaries."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--backend", choices=("both", "cpp", "llvm"), default="both")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    source = fixture / "public_confined_callback.gti"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "platform": platform.platform(), "selected_backend": args.backend,
              "compiler_sha256": sha256(args.compiler.resolve().read_bytes()).hexdigest(),
              "source_sha256": sha256(source.read_bytes()).hexdigest(),
              "native_sha256": sha256((fixture / "public_confined_callback.c").read_bytes()).hexdigest(),
              "harness_sha256": sha256(Path(__file__).read_bytes()).hexdigest(), "steps": []}

    def run(command, tag, expected=0, mode="success"):
        command = [str(item) for item in command]
        start = time.monotonic()
        result = subprocess.run(command, env={**os.environ, "GTI_CONFINED_MODE": mode},
                                capture_output=True, timeout=180)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "mode": mode, "exit": result.returncode,
                                "expected": expected, "seconds": time.monotonic() - start,
                                "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    executions = {"llvm": 0, "cpp": 0}
    for native_level in (0, 3):
        obj = output / f"native-O{native_level}.o"
        run([args.cc, "-std=c17", f"-O{native_level}", "-Wall", "-Wextra", "-Werror",
             "-c", fixture / "public_confined_callback.c", "-o", obj], obj.stem)
        configurations = []
        if args.backend != "cpp":
            configurations += [("llvm", None, level) for level in range(4)]
        if args.backend != "llvm":
            configurations += [("cpp", standard, 3) for standard in ("c++20", "c++23")]
        for backend, standard, level in configurations:
            tag = f"{backend}-{standard}-O{level}-native{native_level}"
            binary = output / tag
            run([args.compiler.resolve(), source, "--backend", backend, "--execution-profile", "concurrent",
                 *(["--std", standard] if standard else []), f"-O{level}", "-o", binary, "--", obj], "build-" + tag)
            for mode, expected in (("success", 0), ("failure", 70), ("double-failure", 70), ("foreign", 86)):
                result = run([binary], tag + "-" + mode, expected, mode)
                if result.stdout:
                    raise RuntimeError("unexpected callback stdout")
                if mode == "success":
                    assert result.stderr == b"gti-cleanup\nnative-returned\n"
                elif mode == "foreign":
                    assert result.stderr.startswith(b"foreign-enter\n")
                    assert b"gti-cleanup" not in result.stderr, "confined callback body ran on foreign thread"
                    assert b"native-returned" not in result.stderr
                else:
                    assert result.stderr.startswith(b"gti-cleanup\n")
                    assert b"native-returned" not in result.stderr
                    assert b"public_confined_callback.gti:" in result.stderr
                    if mode == "double-failure":
                        assert b"GTI-R0014" in result.stderr
                    else:
                        assert b"division" in result.stderr.lower()
                executions[backend] += 1
    unannotated = output / "unannotated.gti"
    unannotated.write_text(source.read_text().replace("[[thread_confined_callback]]", ""))
    for backend in (("llvm", "cpp") if args.backend == "both" else (args.backend,)):
        binary = output / ("unannotated-" + backend)
        binary.write_bytes(b"preserved rejection artifact\n")
        result = run([args.compiler.resolve(), unannotated, "--backend", backend, "--execution-profile",
                      "concurrent", "-O0", "-o", binary], "unannotated-" + backend, 65)
        assert b"GTI-S2076" in result.stderr
        assert binary.read_bytes() == b"preserved rejection artifact\n"
    report["completed"] = True
    report["executions"] = executions
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"public confined callbacks: {executions}; {output}")


if __name__ == "__main__":
    main()
