#!/usr/bin/env python3
"""Strict compiler-cache setup; optional real cold/warm/invalidation evidence."""

import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile


def values(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("script", type=pathlib.Path)
    parser.add_argument("--ccache", type=pathlib.Path)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    script = args.script.resolve()
    bash = shutil.which("bash")
    assert bash
    with tempfile.TemporaryDirectory(prefix="gti-ci-cache-") as temporary:
        root = pathlib.Path(temporary)
        commands = root / "bin"
        commands.mkdir()
        launcher = commands / "ccache"
        launcher.write_text("#!/bin/sh\nexit 0\n")
        launcher.chmod(0o755)
        environment = os.environ.copy()
        environment["PATH"] = str(commands)
        for index, directory in enumerate((str(root / "cache with spaces"), "", "/",
                                           "relative/cache", "/cache\nBAD=value",
                                           "/cache\rBAD=value")):
            output = root / f"environment-{index}"
            environment.update(GITEA_ENV=str(output), GTI_CACHE_DIRECTORY=directory)
            result = subprocess.run([bash, str(script)], env=environment,
                                    capture_output=True, text=True, timeout=10)
            if index:
                assert result.returncode != 0, result
                assert not output.exists(), "invalid cache setup wrote runner state"
                continue
            assert result.returncode == 0, result.stderr
            settings = values(output)
            assert settings == {
                "CMAKE_C_COMPILER_LAUNCHER": str(launcher),
                "CMAKE_CXX_COMPILER_LAUNCHER": str(launcher),
                "CCACHE_DIR": directory, "CCACHE_MAXSIZE": "10G",
                "CCACHE_COMPILERCHECK": "content", "CCACHE_BASEDIR": "",
                "CCACHE_SLOPPINESS": "",
            }
        launcher.unlink()
        output = root / "missing-launcher"
        environment.update(GITEA_ENV=str(output), GTI_CACHE_DIRECTORY=str(root / "cache"))
        result = subprocess.run([bash, str(script)], env=environment,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and not output.exists()

        if args.ccache:
            environment = os.environ.copy()
            environment["PATH"] = str(args.ccache.resolve().parent) + os.pathsep + environment["PATH"]
            output = root / "real-environment"
            environment.update(GITEA_ENV=str(output), GTI_CACHE_DIRECTORY=str(root / "real-cache"))
            subprocess.run([bash, str(script)], env=environment, check=True,
                           capture_output=True, text=True, timeout=10)
            environment.update(values(output))
            header = root / "value.h"
            source = root / "value.cpp"
            artifact = root / "value.o"
            header.write_text("constexpr int value = 17;\n")
            source.write_text('#include "value.h"\nint answer() { return value; }\n')

            def compile(optimization="-O2"):
                subprocess.run([str(args.ccache.resolve()), args.cxx, optimization,
                                "-std=c++20", "-c", str(source), "-o", str(artifact)],
                               env=environment, check=True, capture_output=True,
                               text=True, timeout=60)
                return artifact.read_bytes()

            cold = compile()
            assert compile() == cold, "warm cache changed object bytes"
            header.write_text("constexpr int value = 29;\n")
            assert compile() != cold, "changed header reused a stale object"
            compile("-O0")
            stats = subprocess.run([str(args.ccache.resolve()), "--print-stats"],
                                   env=environment, check=True, capture_output=True,
                                   text=True, timeout=10)
            counts = dict(line.split() for line in stats.stdout.splitlines())
            assert (int(counts.get("direct_cache_hit", 0)) +
                    int(counts.get("preprocessed_cache_hit", 0))) >= 1, stats.stdout
            assert int(counts["cache_miss"]) >= 3, stats.stdout
            print("Real compiler cache: cold miss, byte-identical warm hit, header/flag invalidation passed")
    print("CI compiler-cache configuration and negative cases passed")


if __name__ == "__main__":
    main()
