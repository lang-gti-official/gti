#!/usr/bin/env python3
"""Verify explicit sanitizer runner-capability handling."""

from __future__ import annotations

import os
import sys

from sanitizer_support import thread_sanitizer_enabled


def check(value: str | None, expected: bool) -> None:
    if value is None:
        os.environ.pop("GTI_TSAN_SUPPORTED", None)
    else:
        os.environ["GTI_TSAN_SUPPORTED"] = value

    actual = thread_sanitizer_enabled()
    if actual != expected:
        raise AssertionError(
            f"GTI_TSAN_SUPPORTED={value!r}: expected {expected}, got {actual}"
        )


def main() -> None:
    original = os.environ.get("GTI_TSAN_SUPPORTED")
    try:
        if sys.platform == "win32":
            check(None, False)
            check("0", False)
            check("1", False)
            return

        check(None, True)
        check("0", False)
        check("1", True)

        os.environ["GTI_TSAN_SUPPORTED"] = "invalid"
        try:
            thread_sanitizer_enabled()
        except ValueError:
            pass
        else:
            raise AssertionError("invalid sanitizer capability was accepted")
    finally:
        if original is None:
            os.environ.pop("GTI_TSAN_SUPPORTED", None)
        else:
            os.environ["GTI_TSAN_SUPPORTED"] = original


if __name__ == "__main__":
    main()
