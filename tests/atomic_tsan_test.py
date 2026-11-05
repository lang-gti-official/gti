#!/usr/bin/env python3

"""Run the public atomic concurrency fixtures under ThreadSanitizer when available.

Each fixture is emitted by the LLVM backend and instrumented through the
shared recipe in managed_thread_tsan_test.py; no C++-backend output is
sanitized.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile

from managed_thread_tsan_test import (
    ATOMIC_MARKERS,
    build_instrumented_binary,
    compile_instrumented_runtime,
    run_under_thread_sanitizer,
    thread_sanitizer_available,
)
from sanitizer_support import SKIP_RETURN_CODE, thread_sanitizer_enabled


def main() -> int:
    if len(sys.argv) < 5:
        raise SystemExit(
            "usage: atomic_tsan_test.py /path/to/llvm-instrumented-source "
            "/path/to/matching-clang++ /path/to/reviewed-c++ fixture.gti [fixture.gti ...]"
        )
    if not thread_sanitizer_enabled():
        print("atomic ThreadSanitizer check skipped: unsupported runner")
        return SKIP_RETURN_CODE

    emitter = pathlib.Path(sys.argv[1]).resolve()
    native_compiler = pathlib.Path(sys.argv[2]).absolute()
    runtime_compiler = pathlib.Path(sys.argv[3]).absolute()
    sources = [pathlib.Path(argument).resolve() for argument in sys.argv[4:]]

    with tempfile.TemporaryDirectory(prefix="gti-atomic-tsan-") as scratch:
        root = pathlib.Path(scratch)
        if not (thread_sanitizer_available(native_compiler, root)
                and thread_sanitizer_available(runtime_compiler, root)):
            print(
                "atomic ThreadSanitizer check skipped: native compiler does not "
                "provide -fsanitize=thread"
            )
            return SKIP_RETURN_CODE

        runtime = compile_instrumented_runtime(runtime_compiler, root)
        for source in sources:
            binary = root / source.stem
            build_instrumented_binary(emitter, native_compiler, source, binary,
                                      runtime, markers=ATOMIC_MARKERS,
                                      linker=runtime_compiler)
            run_under_thread_sanitizer(binary)
            print(f"atomic ThreadSanitizer fixture {source.stem} passed")

    print("atomic ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
