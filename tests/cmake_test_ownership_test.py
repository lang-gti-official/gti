#!/usr/bin/env python3
"""Keep product configuration in the root and test registration in tests/."""

from __future__ import annotations

import re
import sys
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

    print("CMake test ownership: root products and test registrations are separate")


if __name__ == "__main__":
    main()
