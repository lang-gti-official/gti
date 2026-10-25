#!/usr/bin/env python3
"""Stage, archive, relocate and consume the LLVM-enabled toolchain and driver."""

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


def run(arguments, cwd, env):
    result = subprocess.run(
        [str(value) for value in arguments], cwd=cwd, env=env,
        capture_output=True, text=True, timeout=300, check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"{arguments}: exit {result.returncode}\n"
            f"stdout:\n{result.stdout[-6000:]}\nstderr:\n{result.stderr[-6000:]}"
        )
    return result


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: llvm_cache_installed_test.py source-root build-root")
    source, build = (Path(value).resolve() for value in sys.argv[1:])
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith(("#", "//")):
            cache[key.split(":", 1)[0]] = value
    assert cache["CMAKE_BUILD_TYPE"] in ("Release", "RelWithDebInfo", "MinSizeRel")
    environment = os.environ.copy()
    for name in ("GTI_STDLIB", "GTI_STDLIB_ROOT", "GTI_STDLIB_PATH", "GTI_RUNTIME_INCLUDE", "GTI_RUNTIME_LIBRARY",
                 "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                 "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET"):
        environment.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="gti-llvm-installed-") as temporary:
        root = Path(temporary).resolve()
        stage = root / "original-stage"
        run(["cmake", "--install", build, "--prefix", stage,
             "--component", "gti_toolchain"], root, environment)
        pure = root / "pure-project"
        pure.mkdir()
        (pure / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "pure-relocation"\n'
            'version = "0.1.0"\n[targets.app]\nkind = "executable"\n'
            'root = "main.gti"\n[profiles.dev]\nbackend = "llvm"\n',
            encoding="utf-8")
        (pure / "main.gti").write_text("int main() { return 0; }\n", encoding="utf-8")
        cold = run([stage / "bin/gti", "build", "--verbose"], pure, environment)
        assert "gti: cache miss " in cold.stderr, cold.stderr
        bundled = cache.get("GTI_BUNDLE_LLVM") == "ON"
        if bundled:
            archive_platform = {("darwin", "arm64"): "darwin-arm64",
                                ("linux", "x86_64"): "linux-x86_64"}[
                                    (sys.platform, platform.machine().lower())]
            packaged = root / "packages"
            version = (source / "VERSION").read_text().strip()
            run(["cmake", f"-DGTI_SOURCE_DIR={source}", f"-DGTI_STAGE_DIR={stage}",
                 f"-DGTI_OUTPUT_DIR={packaged}", f"-DGTI_PLATFORM={archive_platform}",
                 f"-DGTI_EXPECTED_TAG=v{version}",
                 "-P", source / "cmake/PackageRelease.cmake"], root, environment)
            archive, = packaged.glob("*.tar.gz")
            checksum = Path(f"{archive}.sha256").read_text().split()[0]
            with archive.open("rb") as packet:
                assert hashlib.file_digest(packet, "sha256").hexdigest() == checksum
        else:
            archive = root / "gti-toolchain.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                output.add(stage, arcname="gti")
        shutil.rmtree(stage)
        relocated_root = root / "relocated with spaces"
        relocated_root.mkdir()
        prefix = relocated_root / "gti"
        with tarfile.open(archive, "r:gz") as packet:
            if bundled:
                prefix.mkdir()
                packet.extractall(prefix, filter="data")
            else:
                packet.extractall(relocated_root, filter="data")
        gti = prefix / "bin/gti"
        run([gti, "--version"], root, environment)
        if bundled:
            inspector = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
            for binary in (gti, prefix / "bin/gti_lsp"):
                dependencies = run([*inspector, binary], root, environment).stdout
                assert not re.search(r"\blibLLVM[^/\s]*\.(?:so|dylib)", dependencies), dependencies
                assert str(stage) not in dependencies and str(build) not in dependencies
        moved = root / "moved-project"
        pure.rename(moved)
        executable_metadata = json.loads(run([gti, "metadata"], moved, environment).stdout)
        plan = next(value for value in executable_metadata["targets"][0]["outputs"]
                    if value["profile"] == "dev")
        Path(plan["executable"]).unlink()
        restored = run([gti, "build", "--verbose"], moved, environment)
        assert "gti: cache hit " in restored.stderr, restored.stderr
        run([plan["executable"]], moved, environment)
        program = root / "main.gti"
        program.write_text("int main() { return 0; }\n", encoding="utf-8")
        report = root / "build-report.json"
        executable = root / "program"
        run([gti, program, "--backend", "llvm", "-O3", "-o", executable,
             "--build-report", report], root, environment)
        run([executable], root, environment)
        record = json.loads(report.read_text())
        serialized = json.dumps(record)
        assert str(prefix / "lib/libgti_runtime.a") in serialized, serialized
        assert str(prefix / "share/gti/stdlib/prelude.gti") in serialized, serialized
        assert record["native"]["runtime"]["headers"] == [], record["native"]
        assert str(stage) not in serialized and str(build) not in serialized
        run([sys.executable, source / "tests/llvm_cache_project_test.py", gti],
            root, environment)
        consumer = root / "consumer-build"
        configure = ["cmake", "-S", source / "tests/llvm_cache_installed_consumer",
                     "-B", consumer, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_PREFIX_PATH={prefix}"]
        if cache.get("GTI_BUNDLE_LLVM") != "ON" and cache.get("LLVM_DIR"):
            configure.append(f"-DLLVM_DIR={cache['LLVM_DIR']}")
        run(configure, root, environment)
        run(["cmake", "--build", consumer, "--target", "gti_installed_llvm_cache",
             "--parallel", "2"], root, environment)
        run([consumer / "gti_installed_llvm_cache", gti], root, environment)
        coexistence = root / "coexistence-build"
        configure[configure.index("-S") + 1] = source / "tests/installed_library_smoke"
        configure[configure.index("-B") + 1] = coexistence
        run(configure, root, environment)
        run(["cmake", "--build", coexistence, "--target",
             "gti_installed_llvm_clang_coexistence_smoke", "--parallel", "2"],
            root, environment)
        run(["ctest", "--test-dir", coexistence, "--output-on-failure",
             "--no-tests=error", "--parallel", "1",
             "-R", "^installed_llvm_clang_coexistence$"], root, environment)
    channel = "bundled" if cache.get("GTI_BUNDLE_LLVM") == "ON" else "system"
    print(f"Relocated {channel}-LLVM toolchain archive, resource provenance, "
          "project workflows, installed driver cache and LLVM/libclang coexistence passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
