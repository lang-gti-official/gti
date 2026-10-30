#!/usr/bin/env python3

"""Shared runner-capability policy for sanitizer test variants."""

from __future__ import annotations

import os
import sys


SKIP_RETURN_CODE = 77

def scaled_timeout(seconds):
    """A harness's own subprocess bound, scaled like the CTest timeout around it.

    CTest exports GTI_TEST_TIMEOUT_SCALE (tests/TestThroughput.cmake), so a
    slower runner lengthens the per-compile and per-run bounds inside a test
    in step with the test's own TIMEOUT.
    """
    scale = os.environ.get("GTI_TEST_TIMEOUT_SCALE", "1")
    if not scale.isdigit() or int(scale) < 1:
        raise ValueError("GTI_TEST_TIMEOUT_SCALE must be a positive integer")
    return seconds * int(scale)


# GCC's ASan/UBSan instrumentation of a whole-program translation unit is
# slow. One standard-library fixture takes over two minutes uncontended on an
# 8-core Linux host, and more than five under a parallel suite; Clang takes
# far less. Sanitized native builds use this bound instead of an ordinary
# compile timeout.
SANITIZED_BUILD_TIMEOUT = scaled_timeout(1200)


def thread_sanitizer_enabled() -> bool:
    if sys.platform == "win32":
        return False

    configured = os.environ.get("GTI_TSAN_SUPPORTED")
    if configured is None:
        return True
    if configured == "1":
        return True
    if configured == "0":
        return False
    raise ValueError("GTI_TSAN_SUPPORTED must be either 0 or 1")
