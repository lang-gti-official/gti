#!/usr/bin/env python3

"""Run the public atomic concurrency fixture under ThreadSanitizer when available."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import SKIP_RETURN_CODE, thread_sanitizer_enabled
from harness_timeouts import scaled


def execute(
    command: list[str],
    *,
    environment: dict[str, str] | None = None,
    timeout: int = scaled(300),
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
        env=environment,
    )


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: atomic_tsan_test.py /path/to/gti concurrent.gti "
            "orders.gti fetch.gti /path/to/c++"
        )
    if not thread_sanitizer_enabled():
        print("atomic ThreadSanitizer check skipped: unsupported runner")
        return SKIP_RETURN_CODE

    compiler = pathlib.Path(sys.argv[1]).resolve()
    sources = [
        pathlib.Path(sys.argv[2]).resolve(),
        pathlib.Path(sys.argv[3]).resolve(),
        pathlib.Path(sys.argv[4]).resolve(),
    ]
    native_compiler = pathlib.Path(sys.argv[5]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-atomic-tsan-") as scratch:
        root = pathlib.Path(scratch)
        probe_source = root / "probe.cpp"
        probe_binary = root / "probe"
        probe_source.write_text("int main() { return 0; }\n", encoding="utf-8")
        probe = execute(
            [
                str(native_compiler),
                "-std=c++20",
                "-fsanitize=thread",
                str(probe_source),
                "-o",
                str(probe_binary),
            ]
        )
        if probe.returncode != 0:
            print(
                "atomic ThreadSanitizer check skipped: native compiler does not "
                "provide -fsanitize=thread"
            )
            return SKIP_RETURN_CODE

        environment = os.environ.copy()
        environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        for source in sources:
            binary = root / source.stem
            build = execute(
                [
                    str(compiler),
                    "--execution-profile",
                    "concurrent",
                    "--std",
                    "c++23",
                    "-O1",
                    "--cxx",
                    str(native_compiler),
                    str(source),
                    "-o",
                    str(binary),
                    "--",
                    "-fsanitize=thread",
                    "-fno-omit-frame-pointer",
                    "-g",
                ]
            )
            if build.returncode != 0:
                raise AssertionError(
                    f"ThreadSanitizer build returned {build.returncode}\n"
                    f"stdout: {build.stdout}\nstderr: {build.stderr}"
                )
            for _ in range(3):
                run = execute([str(binary)], environment=environment)
                if run.returncode != 0:
                    raise AssertionError(
                        f"ThreadSanitizer run returned {run.returncode}\n"
                        f"stdout: {run.stdout}\nstderr: {run.stderr}"
                    )

    print("atomic ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
