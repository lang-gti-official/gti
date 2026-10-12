#!/usr/bin/env python3

"""Shared runner-capability policy for sanitizer test variants."""

from __future__ import annotations

import os
import sys


SKIP_RETURN_CODE = 77

# GCC's ASan/UBSan instrumentation of a whole-program translation unit is
# slow. One standard-library fixture takes over two minutes uncontended on an
# 8-core Linux host, and more than five under a parallel suite; Clang takes
# far less. Sanitized native builds use this bound instead of an ordinary
# compile timeout.
SANITIZED_BUILD_TIMEOUT = 1200


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
