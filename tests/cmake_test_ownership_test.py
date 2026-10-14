#!/usr/bin/env python3
"""Keep product configuration in the root and test registration in tests/."""

from __future__ import annotations

import re
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"CMake test ownership: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: cmake_test_ownership_test.py <repo-root>")
    root = Path(sys.argv[1]).resolve()
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    test_cmake = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")

    delegation = re.compile(
        r'if\(BUILD_TESTING\)\s+add_subdirectory\("tests"\)\s+endif\(\)',
        re.MULTILINE,
    )
    if not delegation.search(root_cmake):
        fail("the root must delegate BUILD_TESTING to tests/CMakeLists.txt")
    if re.search(r"\b(?:add_test|set_tests_properties)\s*\(", root_cmake):
        fail("the root must not register tests")
    if "${PROJECT_SOURCE_DIR}/tests/" in root_cmake:
        fail("the root must not own test source paths")

    if not re.search(r"\badd_executable\s*\(", test_cmake):
        fail("tests/CMakeLists.txt must own test products")
    if not re.search(r"\badd_test\s*\(", test_cmake):
        fail("tests/CMakeLists.txt must own CTest registration")
    if "CMAKE_CURRENT_SOURCE_DIR" in test_cmake:
        fail("repository resources must use the explicit project root")
    release_layout = re.search(
        r"if\(GTI_RELEASE_BUILD\)\s+"
        r"get_property\(gti_build_tree_tests DIRECTORY PROPERTY TESTS\)\s+"
        r"set_property\(TEST \$\{gti_build_tree_tests\} APPEND PROPERTY ENVIRONMENT"
        r"(?P<environment>.*?)\)\s+endif\(\)\s*$",
        test_cmake, re.DOTALL,
    )
    if release_layout is None:
        fail("release resource overrides must cover the final registered test inventory")
    for resource in ("GTI_STDLIB_PATH", "GTI_RUNTIME_INCLUDE",
                     "GTI_RUNTIME_LIBRARY"):
        if f'"{resource}=' not in release_layout.group("environment"):
            fail(f"release build-tree tests lack {resource}")

    optional_lsp = re.search(
        r"if\(TARGET gti_lsp\)\s+add_test\(\s+NAME structural_relocation_lsp\b"
        r".*?set_tests_properties\(structural_relocation_lsp PROPERTIES TIMEOUT 120\)"
        r"\s+endif\(\)", test_cmake, re.DOTALL,
    )
    if optional_lsp is None:
        fail("structural_relocation_lsp must be conditional on its actual LSP target")
    formatting_lsp = re.search(
        r"if\(TARGET gti_lsp\)\s+add_test\(NAME extensible_formatting_lsp\b"
        r".*?add_test\(NAME integral_enum_lsp\b.*?\s+endif\(\)",
        test_cmake, re.DOTALL,
    )
    if formatting_lsp is None:
        fail("formatting LSP tests must be conditional on their actual LSP target")

    # Execute the actual final CMake fragment in a small standalone project.
    # A newly registered test must inherit the layout too, while existing test
    # environment entries survive and non-release builds remain unaffected.
    with tempfile.TemporaryDirectory(prefix="gti-test-layout-") as temporary:
        source = Path(temporary)
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(Layout NONE)\nenable_testing()\n'
            'add_library(gti_runtime STATIC IMPORTED)\n'
            'set_target_properties(gti_runtime PROPERTIES IMPORTED_LOCATION '
            '"${CMAKE_BINARY_DIR}/runtime.a")\n'
            'add_test(NAME existing COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'set_tests_properties(existing PROPERTIES ENVIRONMENT "SENTINEL=keep")\n'
            'add_test(NAME newly_registered COMMAND "${CMAKE_COMMAND}" -E true)\n'
            + release_layout.group(0), encoding="utf-8",
        )
        for release in ("ON", "OFF"):
            build = source / release
            subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                            f"-DGTI_RELEASE_BUILD={release}"], check=True,
                           capture_output=True, text=True, timeout=30)
            result = subprocess.run(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                check=True, capture_output=True, text=True, timeout=30,
            )
            for test in json.loads(result.stdout)["tests"]:
                properties = {item["name"]: item["value"]
                              for item in test["properties"]}
                environment = properties.get("ENVIRONMENT", [])
                if test["name"] == "existing" and "SENTINEL=keep" not in environment:
                    fail("release layout erased an existing test environment")
                expected = [f"GTI_STDLIB_PATH={source}/stdlib",
                            f"GTI_RUNTIME_INCLUDE={source}/runtime/include",
                            f"GTI_RUNTIME_LIBRARY={build}/runtime.a"]
                for value in expected:
                    if (value in environment) != (release == "ON"):
                        fail(f"{release}/{test['name']}: incorrect override {value}")

        # Evaluate the owning registration with the optional target both absent
        # and present. In the OFF case even generation must succeed, not just
        # configuring before TARGET_FILE generator expressions are evaluated.
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(OptionalLsp NONE)\nenable_testing()\n'
            'if(ENABLE_LSP)\nadd_executable(gti_lsp IMPORTED GLOBAL)\n'
            'set_target_properties(gti_lsp PROPERTIES IMPORTED_LOCATION '
            '"${CMAKE_COMMAND}")\nendif()\n' + optional_lsp.group(0)
            + '\n' + formatting_lsp.group(0),
            encoding="utf-8",
        )
        for enabled in ("ON", "OFF"):
            build = source / f"lsp-{enabled}"
            subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                            f"-DENABLE_LSP={enabled}"], check=True,
                           capture_output=True, text=True, timeout=30)
            result = subprocess.run(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                check=True, capture_output=True, text=True, timeout=30,
            )
            names = [test["name"] for test in json.loads(result.stdout)["tests"]]
            expected_names = ["structural_relocation_lsp", "extensible_formatting_lsp",
                              "integral_enum_lsp", "generic_enum_lsp"] if enabled == "ON" else []
            if names != expected_names:
                fail(f"LSP {enabled}: incorrect optional test inventory {names}")

    print("CMake test ownership: root products and test registrations are separate")


if __name__ == "__main__":
    main()
