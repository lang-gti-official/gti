#!/usr/bin/env python3
"""Verify that every test- or fuzz-enabled GTI build is optimized."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path
from harness_timeouts import scaled


def fail(message: str) -> None:
    raise SystemExit(f"Optimized test compiler policy: {message}")


def configure(
    source: Path,
    build: Path,
    *definitions: str,
    succeeds: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["cmake", "-S", str(source), "-B", str(build), *definitions],
        capture_output=True,
        text=True,
        timeout=scaled(30),
    )
    if succeeds and result.returncode != 0:
        fail(f"configuration unexpectedly failed:\n{result.stdout}{result.stderr}")
    if not succeeds and result.returncode == 0:
        fail("configuration unexpectedly accepted an unoptimized test compiler")
    return result


def read_configuration(build: Path) -> tuple[str, str]:
    lines = (build / "selected-configurations.txt").read_text(
        encoding="utf-8"
    ).splitlines()
    if len(lines) != 2:
        fail("policy probe did not record both configuration fields")
    return lines[0], lines[1]


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: optimized_test_compiler_policy_test.py <repo-root>")

    root = Path(sys.argv[1]).resolve()
    module = root / "cmake/OptimizedTestCompiler.cmake"
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    include_text = (
        'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OptimizedTestCompiler.cmake")'
    )
    call_text = "gti_require_optimized_test_compiler()"
    if include_text not in root_cmake or call_text not in root_cmake:
        fail("the root CMake project does not apply the test compiler policy")
    if root_cmake.index(call_text) > root_cmake.index(
        "find_package(Python3 COMPONENTS Interpreter"
    ):
        fail("the test compiler policy must run before dependency discovery")

    with tempfile.TemporaryDirectory(
        prefix="gti-optimized-test-policy-"
    ) as temporary:
        source = Path(temporary) / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(OptimizedTestCompilerPolicy NONE)\n"
            "if(SIMULATE_MULTI_CONFIG)\n"
            '  set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")\n'
            "endif()\n"
            f'include("{module.as_posix()}")\n'
            "gti_require_optimized_test_compiler()\n"
            'file(WRITE "${CMAKE_BINARY_DIR}/selected-configurations.txt"\n'
            '     "${CMAKE_BUILD_TYPE}\\n${CMAKE_CONFIGURATION_TYPES}\\n")\n',
            encoding="utf-8",
        )

        for configuration in ("Release", "RelWithDebInfo", "MinSizeRel"):
            build = Path(temporary) / configuration
            configure(
                source,
                build,
                "-DBUILD_TESTING=ON",
                f"-DCMAKE_BUILD_TYPE={configuration}",
            )
            selected, _ = read_configuration(build)
            if selected != configuration:
                fail(f"{configuration} changed to {selected!r}")

        default_build = Path(temporary) / "default"
        configure(source, default_build, "-DBUILD_TESTING=ON")
        selected, _ = read_configuration(default_build)
        if selected != "Release":
            fail(f"an unspecified test build defaulted to {selected!r}, not Release")

        debug_build = Path(temporary) / "debug"
        result = configure(
            source,
            debug_build,
            "-DBUILD_TESTING=ON",
            "-DCMAKE_BUILD_TYPE=Debug",
            succeeds=False,
        )
        output = result.stdout + result.stderr
        if (
            "GTI tests and fuzzers require an optimized compiler configuration"
            not in output
        ):
            fail(
                "Debug rejection did not explain the optimized compiler requirement"
            )

        debug_without_tests = Path(temporary) / "debug-without-tests"
        configure(
            source,
            debug_without_tests,
            "-DBUILD_TESTING=OFF",
            "-DCMAKE_BUILD_TYPE=Debug",
        )
        selected, _ = read_configuration(debug_without_tests)
        if selected != "Debug":
            fail("a test-disabled debugger build was unexpectedly changed")

        debug_fuzzer_build = Path(temporary) / "debug-fuzzers"
        configure(
            source,
            debug_fuzzer_build,
            "-DBUILD_TESTING=OFF",
            "-DGTI_BUILD_FUZZERS=ON",
            "-DCMAKE_BUILD_TYPE=Debug",
            succeeds=False,
        )

        multi_build = Path(temporary) / "multi"
        configure(
            source,
            multi_build,
            "-DBUILD_TESTING=ON",
            "-DSIMULATE_MULTI_CONFIG=ON",
        )
        _, configurations = read_configuration(multi_build)
        if configurations != "Release;RelWithDebInfo;MinSizeRel":
            fail(f"multi-config test build retained {configurations!r}")

    print(
        "Optimized test compiler policy: all test configurations are optimized"
    )


if __name__ == "__main__":
    main()
