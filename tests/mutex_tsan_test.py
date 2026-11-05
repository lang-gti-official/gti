#!/usr/bin/env python3

"""Run the public mutex contention fixture under ThreadSanitizer when available.

The fixture is emitted by the LLVM backend and instrumented through the shared
recipe in managed_thread_tsan_test.py; no C++-backend output is sanitized.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile

from managed_thread_tsan_test import (
    build_instrumented_binary,
    compile_instrumented_runtime,
    run_under_thread_sanitizer,
    thread_sanitizer_available,
)
from sanitizer_support import SKIP_RETURN_CODE, thread_sanitizer_enabled


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: mutex_tsan_test.py /path/to/llvm-instrumented-source "
            "/path/to/matching-clang++ /path/to/reviewed-c++ mutex_concurrent.gti"
        )
    if not thread_sanitizer_enabled():
        print("mutex ThreadSanitizer check skipped: unsupported runner")
        return SKIP_RETURN_CODE

    emitter = pathlib.Path(sys.argv[1]).resolve()
    native_compiler = pathlib.Path(sys.argv[2]).absolute()
    runtime_compiler = pathlib.Path(sys.argv[3]).absolute()
    source = pathlib.Path(sys.argv[4]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-mutex-tsan-") as scratch:
        root = pathlib.Path(scratch)
        if not (thread_sanitizer_available(native_compiler, root)
                and thread_sanitizer_available(runtime_compiler, root)):
            print(
                "mutex ThreadSanitizer check skipped: native compiler does not "
                "provide -fsanitize=thread"
            )
            return SKIP_RETURN_CODE

        binary = root / source.stem
        runtime = compile_instrumented_runtime(runtime_compiler, root)
        build_instrumented_binary(emitter, native_compiler, source, binary,
                                  runtime, linker=runtime_compiler)
        run_under_thread_sanitizer(binary)

    print("mutex ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
