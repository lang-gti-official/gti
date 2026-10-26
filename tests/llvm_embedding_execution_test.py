#!/usr/bin/env python3
"""Verified infallible GTI exports loaded by real C17 and C++ hosts."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import platform
import tempfile
import subprocess
import sys
import time
from llvm_evidence_manifest import build_manifest, build_context_sidecar
from llvm_case_context import CaseContext

SOURCE = "tests/fixtures/embedding/infallible.gti"
EXPORTS = ["exchange", "mix", "pair", "large", "hfa", "engine", "record",
           "nothing", "tiny", "small", "real", "fixed", "signed_short",
           "signed_word", "signed_long", "unsigned_byte", "unsigned_word", "single"]


def evidence_cases(root):
    return {"infallible": (root / SOURCE).read_text()}


def evidence_profiles(root):
    del root
    return {"infallible": ("infallible",)}


def evidence_program_requests(root):
    del root
    return {"infallible": {"kind": "library", "exports": [
        {"function": name, "symbol": "embed_" + name} for name in EXPORTS]}}


def evidence_manifest(root):
    return build_manifest("llvm_embedding_execution", evidence_cases(root),
                          evidence_profiles(root), {"infallible": SOURCE},
                          program_requests=evidence_program_requests(root))


def evidence_partition_policy(root):
    del root
    return {"cases": "positive-admission", "controls": "expected-rejection-control"}


def evidence_context(root):
    context = CaseContext("llvm_embedding_execution", "infallible", SOURCE,
                          sha256((root / SOURCE).read_bytes()).hexdigest(),
                          "single-threaded", "library-export-program-request",
                          "native-shared-library-dynamic-host", ())
    return build_context_sidecar(evidence_manifest(root), {"infallible": {
        "context": context.record(), "role": "positive-admission", "observation": {
            "setup": {"program-request": evidence_program_requests(root)["infallible"],
                      "optimization": [0, 1, 2, 3], "hosts": ["c17", "c++20", "c++23"]},
            "endpoints": ["create", "invoke", "destroy", "unload", "reload"],
            "expectation": {"exit": 0, "reload_cycles": 3, "export_count": len(EXPORTS)}
        }}}, {}, partition_policy=evidence_partition_policy(root))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixture = root / "tests/fixtures/embedding"
    includes = ["-I", str(root / "runtime/include")]
    emitter = args.emitter.resolve()
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "platform": platform.platform(), "emitter_sha256": sha256(emitter.read_bytes()).hexdigest(),
              "source_sha256": sha256((root / SOURCE).read_bytes()).hexdigest(), "steps": []}

    def run(command, tag, expected=0):
        command = [str(item) for item in command]
        start = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "exit": result.returncode,
                                "expected": expected, "seconds": time.monotonic() - start,
                                "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    expected_symbols = {"gti_get_module_descriptor_v1", "gti_context_create_v1",
                        "gti_context_destroy_v1", *("embed_" + name for name in EXPORTS)}
    for level in range(4):
        directory = Path(tempfile.mkdtemp(prefix=f"O{level}-", dir=output))
        stem = directory / "module"
        run([emitter, root / SOURCE, stem, level, args.cxx, args.runtime.resolve(),
             *(f"{name}=embed_{name}" for name in EXPORTS)], f"generate-O{level}")
        generation = directory / "module-generation"
        library = generation / ("libmodule.dylib" if sys.platform == "darwin" else "libmodule.so")
        assert {path.name for path in generation.iterdir()} == {library.name, "module.h"}
        assert (generation / "module.h").read_bytes() == stem.with_suffix(".h").read_bytes()
        if level == 0:
            original = {path.name: sha256(path.read_bytes()).hexdigest() for path in generation.iterdir()}
            duplicate = run([emitter, root / SOURCE, stem, level, args.cxx, args.runtime.resolve(),
                             *(f"{name}=embed_{name}" for name in EXPORTS)], "existing-generation", 1)
            assert b"already exists" in duplicate.stderr
            assert original == {path.name: sha256(path.read_bytes()).hexdigest() for path in generation.iterdir()}
            blocked = directory / "blocked-generation"
            blocked.symlink_to(generation, target_is_directory=True)
            run([emitter, root / SOURCE, directory / "blocked", level, args.cxx, args.runtime.resolve(),
                 *(f"{name}=embed_{name}" for name in EXPORTS)], "symlink-generation", 1)
            assert blocked.is_symlink()
            failed = directory / "failed"
            run([emitter, root / SOURCE, failed, level, "/usr/bin/false", args.runtime.resolve(),
                 *(f"{name}=embed_{name}" for name in EXPORTS)], "failed-link", 1)
            assert not (directory / "failed-generation").exists()
            assert not list(directory.glob("*stage*"))
            # Valid C symbols may collide with source record names. Header
            # representation names must avoid that collision in both languages.
            collision_stem = directory / "collision"
            run([emitter, root / SOURCE, collision_stem, level, args.cxx, args.runtime.resolve(),
                 "pair=Pair"], "record-symbol-collision")
            for standard in ("c17", "c++20", "c++23"):
                run([args.cc if standard == "c17" else args.cxx,
                     "-x", "c" if standard == "c17" else "c++", "-std=" + standard,
                     "-Werror", "-fsyntax-only", *includes,
                     directory / "collision-generation/module.h"], "collision-" + standard)
        report.setdefault("artifacts", []).append({
            "level": level, "library": str(library), "bytes": library.stat().st_size,
            "sha256": sha256(library.read_bytes()).hexdigest(),
            "header_sha256": sha256((generation / "module.h").read_bytes()).hexdigest()})
        symbols = run(["nm", "-gU" if sys.platform == "darwin" else "-D", library],
                      f"symbols-O{level}").stdout.decode()
        defined = {line.split()[-1].removeprefix("_") if sys.platform == "darwin" else line.split()[-1]
                   for line in symbols.splitlines() if len(line.split()) >= 3 and line.split()[-2] in ("T", "D", "B", "W", "V")}
        if defined != expected_symbols:
            raise RuntimeError(f"unexpected public symbols: {defined ^ expected_symbols}")
        for standard in ("c17", "c++20", "c++23"):
            compiler = args.cc if standard == "c17" else args.cxx
            host = directory / ("host-" + standard)
            run([compiler, "-x", "c" if standard == "c17" else "c++", "-std=" + standard,
                 "-O3", "-Wall", "-Wextra", "-Werror", *includes, "-I", generation,
                 fixture / "host.c", "-pthread", *([] if sys.platform == "darwin" else ["-ldl"]),
                 "-o", host], f"host-O{level}-{standard}")
            run([host, library], f"execute-O{level}-{standard}")
        if level == 0:
            changed_source = directory / "identity.gti"
            original_text = (root / SOURCE).read_text()
            assert "value ^ 91" in original_text
            identities = []
            for constant in ("91", "90"):
                changed_source.write_text(original_text.replace("value ^ 91", "value ^ " + constant))
                changed_stem = directory / ("identity-" + constant)
                run([emitter, changed_source, changed_stem, level, args.cxx, args.runtime.resolve(),
                     *(f"{name}=embed_{name}" for name in EXPORTS)], "identity-generate-" + constant)
                changed_library = directory / ("identity-" + constant + "-generation") / library.name
                identities.append(run([host, changed_library, "--identity"], "identity-observe-" + constant).stdout)
            assert identities[0] != identities[1], "infallible code mutation did not change module identity"
    for name, selected in (("reject_fallible", "divide"), ("reject_native", "call")):
        stem = output / name
        stem.with_suffix(".o").write_bytes(b"preserved object")
        stem.with_suffix(".h").write_bytes(b"preserved header")
        rejection = run([emitter, fixture / (name + ".gti"), stem, "0", args.cxx,
                         args.runtime.resolve(), selected + "=embed_reject"], name, 1)
        if b"ProgramKind.Library (O-LLVM-11)" not in rejection.stderr:
            raise RuntimeError(f"{name}: not rejected by embedding closure admission")
        assert stem.with_suffix(".o").read_bytes() == b"preserved object"
        assert stem.with_suffix(".h").read_bytes() == b"preserved header"
    print("embedding: 12 dynamic C/C++ hosts x 3 reload cycles; 2 admission negatives passed")


if __name__ == "__main__":
    main()
