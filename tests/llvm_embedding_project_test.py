#!/usr/bin/env python3
"""Normal project build, matched header, cache, immutable generations and hosts."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    project = Path(tempfile.mkdtemp(prefix="project-", dir=output))
    shutil.copytree(root / "examples/infallible-embedding", project, dirs_exist_ok=True)
    compiler = args.compiler.resolve()
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(), "project": str(project), "steps": []}

    def run(arguments, name, success=True):
        start = time.monotonic()
        result = subprocess.run([str(arg) for arg in arguments], cwd=project,
                                capture_output=True, timeout=180)
        log = output / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": [str(arg) for arg in arguments], "exit": result.returncode,
                                "seconds": time.monotonic() - start, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        assert (result.returncode == 0) == success, f"{name}: exit {result.returncode}; {log}"
        return result

    extension = ".dylib" if sys.platform == "darwin" else ".so"
    def libraries():
        return {path for path in (project / "build/gti").rglob("libmodule" + extension)
                if path.parent.name.endswith(".generation")}

    command = [compiler, "build", "module", "--backend", "llvm", "--release", "--verbose"]
    def build(name, extra=()):
        before = libraries()
        result = run([*command, *extra], name)
        added = libraries() - before
        assert len(added) == 1, f"expected one new immutable generation: {added}"
        binary = added.pop()
        assert (binary.parent / "module.h").is_file()
        return binary, result

    first, _ = build("cold")
    second, hit = build("cache-hit")
    assert b"cache hit" in hit.stderr.lower(), hit.stderr
    assert first != second and first.read_bytes() == second.read_bytes()
    initial = {str(path): sha256(path.read_bytes()).hexdigest() for path in libraries()}
    headers = list((project / "build/gti").rglob("embedding-v1/*/embedding.h"))
    assert len(headers) == 1
    headers[0].write_text("corrupt cache companion\n")
    third, recovered = build("cache-corruption")
    assert b"corrupt" in recovered.stderr.lower()
    assert (third.parent / "module.h").read_bytes() == (first.parent / "module.h").read_bytes()
    headers[0].unlink()
    _, missing = build("cache-missing-companion")
    assert b"corrupt" in missing.stderr.lower()
    headers[0].unlink()
    headers[0].symlink_to(first.parent / "module.h")
    _, linked = build("cache-symlink-companion")
    assert b"corrupt" in linked.stderr.lower()
    assert not headers[0].is_symlink()
    provenance = output / "build-report.json"
    fourth, _ = build("report", ["--build-report", provenance])
    metadata = json.loads(provenance.read_text())
    assert Path(metadata["artifact"]["canonical_path"]) == fourth
    header = fourth.parent / "module.h"
    assert Path(metadata["embedding_header"]["canonical_path"]) == header
    assert metadata["embedding_header"]["sha256"] == sha256(header.read_bytes()).hexdigest()
    run([compiler, "verify", "--backend", "llvm", "--build-report", provenance], "verify-report")
    for standard in ("c17", "c++20", "c++23"):
        host = output / ("host-" + standard)
        run([args.cc if standard == "c17" else args.cxx, "-x", "c" if standard == "c17" else "c++",
             "-std=" + standard, "-O3", "-Wall", "-Wextra", "-Werror", "-I", fourth.parent,
             "-I", root / "runtime/include", project / "host.c",
             *([] if sys.platform == "darwin" else ["-ldl"]), "-o", host], "host-" + standard)
        for index, binary in enumerate((first, second, third, fourth)):
            run([host, binary], f"execute-{standard}-{index}")
    before = libraries()
    run([compiler, "run", "module", "--backend", "llvm"], "reject-run", False)
    run([compiler, "test", "module", "--backend", "llvm"], "reject-test", False)
    run([compiler, "build", "module", "--backend", "cpp"], "reject-cpp", False)
    assert libraries() == before
    source = project / "src/module.gti"
    original = source.read_text()
    source.write_text(original.replace("return previous", "return previous + next"))
    rejected = run(command, "reject-fallible", False)
    assert b"GTI-B0003" in rejected.stderr
    assert libraries() == before
    source.write_text(original)
    failed = run([*command, "--cxx", "/usr/bin/false", "--no-cache"], "reject-link", False)
    assert b"embedding native link failed" in failed.stderr
    assert libraries() == before
    source.write_text(original.replace("return 17", "return 18"))
    _, changed = build("changed-source")
    assert b"cache miss" in changed.stderr.lower()
    source.write_text(original)
    _, restored = build("restored-source")
    assert b"cache hit" in restored.stderr.lower()
    manifest = project / "gti.toml"
    original_manifest = manifest.read_text()
    manifest.write_text(original_manifest.replace("sample_echo", "sample_echo_v2"))
    changed_library, changed = build("changed-exports")
    assert b"cache miss" in changed.stderr.lower()
    assert b"sample_echo_v2" in (changed_library.parent / "module.h").read_bytes()
    manifest.write_text(original_manifest)
    _, restored = build("restored-exports")
    assert b"cache hit" in restored.stderr.lower()
    manifest.write_text(original_manifest + '\n[profiles.dev]\nbackend = "llvm"\noptimization = 0\ndebug-info = true\n')
    command = [compiler, "build", "module", "--verbose"]
    debug, cold = build("debug-cold")
    assert b"cache miss" in cold.stderr.lower()
    debug_hit, hit = build("debug-hit")
    assert b"cache hit" in hit.stderr.lower()
    assert debug.read_bytes() == debug_hit.read_bytes()
    for index, binary in enumerate((debug, debug_hit)):
        run([output / "host-c17", binary], f"execute-debug-{index}")
        if sys.platform == "darwin":
            bundle = Path(str(binary) + ".dSYM")
            assert (bundle / ".gti-debug-symbols").read_text() == "gti-debug-symbols-v1\n"
            native_uuid = run(["dwarfdump", "--uuid", binary], f"debug-native-uuid-{index}").stdout
            bundle_uuid = run(["dwarfdump", "--uuid", bundle], f"debug-bundle-uuid-{index}").stdout
            assert re.findall(rb"UUID: ([A-Fa-f0-9-]+)", native_uuid) == re.findall(rb"UUID: ([A-Fa-f0-9-]+)", bundle_uuid)
            lines = run(["dwarfdump", "--debug-line", bundle], f"debug-lines-{index}").stdout
        else:
            lines = run(["readelf", "--debug-dump=decodedline", binary], f"debug-lines-{index}").stdout
        assert b"module.gti" in lines
    manifest.write_text(original_manifest)
    assert all(sha256(Path(path).read_bytes()).hexdigest() == digest for path, digest in initial.items())
    run([compiler, "clean"], "clean")
    assert not libraries()
    report["completed"] = True
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"project embedding: immutable generations, cache corruption, report, 14 hosts, debug companions, rejection and clean; {project}")


if __name__ == "__main__":
    main()
