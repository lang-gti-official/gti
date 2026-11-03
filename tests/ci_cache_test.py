#!/usr/bin/env python3
"""Strict compiler-cache setup; optional real cold/warm/invalidation evidence."""

import argparse
import os
import pathlib
import re
import json
import shutil
import subprocess
import tempfile


def values(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines())



def warmup_contract(repository, bash):
    workflow = (repository / ".github/workflows/cache.yml").read_text()
    def block(name):
        return workflow.split("      - name: " + name + "\n", 1)[1].split("      - ", 1)[0]
    def step(name):
        lines = block(name).split("        run: |\n", 1)[1].splitlines()
        return "\n".join(line[10:] for line in lines if line.startswith("          "))
    build = step("Build compiler cache inputs")
    cleanup = step("Release warmup scratch space before cache upload")
    assert workflow.index("run: ccache --cleanup") < workflow.index("name: Build compiler cache inputs")
    assert workflow.index("name: Release warmup scratch space") < workflow.index("name: Save the cache under main")
    assert "if: always()" in block("Release warmup scratch space before cache upload")
    # Main owns the reusable native/sanitizer caches and writes them only from a
    # warmup whose build ran to completion; validation runs only restore them.
    assert "id: build" in block("Build compiler cache inputs")
    save = block("Save the cache under main")
    assert re.search(r"if: steps\.build\.outcome == 'success' && steps\.cache\.outputs\.cache-hit != 'true'", save), save
    for validation in ("ci.yml", "full-validation.yml"):
        text = (repository / ".github/workflows" / validation).read_text()
        assert "actions/cache/save@" not in text, f"{validation} must only restore the main caches"
    for profile, build_dir, expected in (("native", "build", ["gti", "gti_lsp"]),
                                         ("sanitizers", "build-sanitized", ["gti"])):
        with tempfile.TemporaryDirectory(prefix="gti-cache-warmup-") as temporary:
            root = pathlib.Path(temporary)
            commands = root / "bin"; commands.mkdir()
            log = root / "commands.jsonl"
            for command in ("cmake", "ccache"):
                fake = commands / command
                fake.write_text("#!" + shutil.which("python3") + "\nimport json, os, sys\n"
                                "with open(os.environ['COMMAND_LOG'], 'a') as output: output.write(json.dumps(sys.argv) + '\\n')\n")
                fake.chmod(0o755)
            cache = root / "cache"; cache.mkdir(); (cache / "retained.o").write_text("cached object")
            scratch = root / build_dir; scratch.mkdir(); (scratch / "discarded.o").write_text("scratch object")
            environment = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                               COMMAND_LOG=str(log), GTI_JOBS="2", CCACHE_DIR=str(cache), RUNNER_TEMP=str(root),
                               GTI_CACHE_DIRECTORY=str(cache))
            for script in (build, cleanup):
                script = script.replace("${{ matrix.profile }}", profile).replace("${{ matrix.build }}", build_dir)
                subprocess.run([bash, "-euo", "pipefail", "-c", script], cwd=root, env=environment,
                               check=True, capture_output=True, text=True, timeout=10)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            assert calls[0][1:] == ["--build", build_dir, "--parallel", "2", "--target", *expected], calls
            assert calls[1][1:] == ["--cleanup"], calls
            assert not scratch.exists() and (cache / "retained.o").read_text() == "cached object"
    print("Warmup builds only compiler/LSP targets; scratch cleanup preserves cache; cache writers are scoped")


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
        environment.pop("GITEA_ENV", None)
        environment["PATH"] = str(commands)
        for index, directory in enumerate((str(root / "cache with spaces"), "", "/",
                                           "relative/cache", "/cache\nBAD=value",
                                           "/cache\rBAD=value")):
            output = root / f"environment-{index}"
            environment.update(GITHUB_ENV=str(output), GTI_CACHE_DIRECTORY=directory)
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
                "CCACHE_DIR": directory, "CCACHE_MAXSIZE": "2G",
                "CCACHE_COMPILERCHECK": "content", "CCACHE_BASEDIR": "",
                "CCACHE_SLOPPINESS": "",
            }
        gitea_output, ignored_legacy = root / "gitea-environment", root / "unused-github-environment"
        environment.update(GITEA_ENV=str(gitea_output), GITHUB_ENV=str(ignored_legacy),
                           GTI_CACHE_DIRECTORY=str(root / "gitea-cache"))
        subprocess.run([bash, str(script)], env=environment, check=True,
                       capture_output=True, text=True, timeout=10)
        assert values(gitea_output)["CCACHE_DIR"] == str(root / "gitea-cache")
        assert not ignored_legacy.exists(), "Gitea must use its own runner environment file"
        environment.pop("GITEA_ENV")
        launcher.unlink()
        output = root / "missing-launcher"
        environment.update(GITHUB_ENV=str(output), GTI_CACHE_DIRECTORY=str(root / "cache"))
        result = subprocess.run([bash, str(script)], env=environment,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and not output.exists()

        if args.ccache:
            environment = os.environ.copy()
            environment.pop("GITEA_ENV", None)
            environment["PATH"] = str(args.ccache.resolve().parent) + os.pathsep + environment["PATH"]
            output = root / "real-environment"
            environment.update(GITHUB_ENV=str(output), GTI_CACHE_DIRECTORY=str(root / "real-cache"))
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
    warmup_contract(script.parents[1], bash)
    print("CI compiler-cache configuration and negative cases passed")


if __name__ == "__main__":
    main()
