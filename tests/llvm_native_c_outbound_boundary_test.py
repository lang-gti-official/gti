#!/usr/bin/env python3
"""C shim contracts and labeled foreign-exception fault injection, not C04 admission."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    source = fixture / "outbound_exception_boundary.gti"
    helper = fixture / "outbound_exception_boundary.cpp"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    cxx = shutil.which(args.cxx) or args.cxx
    environment = {**os.environ, "GTI_CXX": cxx, "CXX": cxx}
    report = {
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
        "platform": platform.platform(),
        "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(),
        "input_sha256": {str(path.relative_to(root)): sha256(path.read_bytes()).hexdigest()
                         for path in (source, helper, Path(__file__).resolve())},
        "contract": "outbound shims contain C++ exceptions; callbacks terminate native exceptions",
        "c04_transport_evidence": False,
        "steps": [], "observations": [],
    }

    def save():
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(argv, tag, expected=0, extra_env=None):
        command = [str(part) for part in argv]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180,
                                env={**environment, **(extra_env or {})})
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "env": extra_env or {},
                                "exit": result.returncode, "expected": expected,
                                "seconds": time.monotonic() - started, "log": str(log)})
        save()
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: expected {expected}, got {result.returncode}; {log}")
        return result

    run([cxx, "--version"], "cxx-version")
    run([compiler, source, "--emit-native-header", "-o",
         output / "outbound_exception_boundary.h"], "native-header")
    run([compiler, source, "--emit-mir", "-o", output / "boundary.mir"], "mir")
    executions = violations = 0
    for host_level in (0, 3):
        obj = output / f"host-O{host_level}.o"
        run([cxx, "-std=c++20", f"-O{host_level}", "-Wall", "-Wextra", "-Werror",
             "-I", output, "-I", root / "runtime/include", "-c", helper, "-o", obj], obj.stem)
        for backend, standard, level in [*(('llvm', 'c++20', n) for n in range(4)),
                                         ('cpp', 'c++20', 3), ('cpp', 'c++23', 3)]:
            tag = f"{backend}-{standard}-O{level}-host{host_level}"
            binary = output / tag
            standard_args = ["--std", standard] if backend == "cpp" else []
            run([compiler, source, "--backend", backend, *standard_args, f"-O{level}",
                 "-o", binary, "--", obj], "build-" + tag)
            for path in ("direct", "indirect"):
                for mode in range(5):
                    case = f"{tag}-{path}-case{mode}"
                    result = run([binary], case, 0 if mode < 2 else 70 if mode == 4 else 86,
                                 {"GTI_NATIVE_BOUNDARY_MODE": str(mode),
                                  "GTI_NATIVE_BOUNDARY_PATH": path})
                    if result.stdout:
                        raise RuntimeError(f"{case}: unexpected stdout")
                    if mode < 2:
                        expected = (b"foreign-cleanup\n" +
                                    (b"translated-in-shim\n" if mode else b"") +
                                    b"gti-cleanup-2\ngti-cleanup-1\n" +
                                    (b"returned-through-C:-7\n" if mode else b"returned-through-C:41\n"))
                        if result.stderr != expected:
                            raise RuntimeError(f"{case}: result or ordered cleanup changed")
                    elif mode == 2:
                        if result.stderr != b"foreign-cleanup\nterminal-in-shim\n":
                            raise RuntimeError(f"{case}: shim terminal boundary changed")
                    elif mode == 3:
                        # Only callback terminal non-escape is a contract here.
                        # GTI cleanup during a violating foreign unwind is an
                        # observation, not a promised outcome or coverage credit.
                        if (not result.stderr.startswith(b"foreign-cleanup\n") or
                                b"escaped-callback" in result.stderr or
                                b"returned-through-C" in result.stderr):
                            raise RuntimeError(f"{case}: foreign fault escaped callback")
                        violations += 1
                    else:
                        expected = b"foreign-cleanup\ntranslated-in-shim\ngti-cleanup-2\ngti-cleanup-1\n"
                        if (not result.stderr.startswith(expected) or b"GTI-R" not in result.stderr or
                                b"division" not in result.stderr.lower() or
                                b"outbound_exception_boundary.gti:" not in result.stderr or
                                b"returned-through-C" in result.stderr or
                                b"escaped-callback" in result.stderr):
                            raise RuntimeError(f"{case}: translated result lost GTI failure cleanup or attribution")
                    report["observations"].append({
                        "case": case, "backend": backend, "mode": mode, "path": path,
                        "supported_shim": mode in (0, 1, 4), "contract_violation": mode == 3,
                        "stderr": result.stderr.decode(), "executable_bytes": binary.stat().st_size,
                        "binary_sha256": sha256(binary.read_bytes()).hexdigest(),
                    })
                    executions += 1
                    save()
    report.update({"completed": True, "executions": executions,
                   "contract_violation_observations": violations})
    save()
    print(f"PASS: {executions} native boundary cases, including {violations} labeled fault observations; {output}")


if __name__ == "__main__":
    main()
