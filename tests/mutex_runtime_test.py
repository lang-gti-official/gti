#!/usr/bin/env python3

"""Exercise scoped mutex ownership, contention, and failure cleanup."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


FAILURE_CLEANUP_SOURCE = """\
#include <std/atomic>
#include <std/mutex>
#include <std/thread>

std::mutex<int32_t> protected_value{0};
std::atomic<bool> worker_locked{false};

int main() {
  auto worker_started = std::thread::start([]() -> void {
    mut std::mutex_guard<int32_t> guard = protected_value.lock();
    worker_locked.store(true);
    int32_t zero = 0;
    int32_t failed = 1 / zero;
  });
  if (!worker_started.has_value()) { return 1; }
  mut std::thread worker = std::move(worker_started).value();

  while (!worker_locked.load()) {}
  auto waiter_started = std::thread::start([]() -> void {
    std::mutex_guard<int32_t> guard = protected_value.lock();
    std::println("waiter acquired");
  });
  if (!waiter_started.has_value()) { return 2; }
  mut std::thread waiter = std::move(waiter_started).value();

  auto worker_joined = worker.join();
  return 3;
}
"""


def execute(
    command: list[str],
    *,
    expected: int = 0,
    timeout: int = scaled(300),
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {command}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def executable_path(root: pathlib.Path, name: str) -> pathlib.Path:
    return root / (f"{name}.exe" if sys.platform == "win32" else name)


def build(
    compiler: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    *,
    standard: str,
    optimization: str,
    native_arguments: list[str] | None = None,
) -> None:
    command = [
        str(compiler),
        "--execution-profile",
        "concurrent",
        "--std",
        standard,
        optimization,
        str(source),
        "-o",
        str(output),
    ]
    if native_arguments:
        command.extend(["--", *native_arguments])
    execute(command)


def source_line(source: str, needle: str) -> int:
    return next(
        index
        for index, line in enumerate(source.splitlines(), start=1)
        if needle in line
    )


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: mutex_runtime_test.py /path/to/gti basic.gti "
            "concurrent.gti move_only.gti"
        )
    requested = pathlib.Path(sys.argv[1])
    resolved = requested.resolve() if requested.exists() else shutil.which(sys.argv[1])
    if resolved is None:
        raise AssertionError(f"could not resolve GTI executable: {sys.argv[1]}")
    compiler = pathlib.Path(resolved).resolve()
    sources = [pathlib.Path(argument).resolve() for argument in sys.argv[2:5]]

    with tempfile.TemporaryDirectory(prefix="gti-mutex-") as scratch:
        root = pathlib.Path(scratch)
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O3"):
                suffix = f"{standard.replace('+', 'p')}_{optimization[1:].lower()}"
                for source in sources:
                    binary = executable_path(root, f"{source.stem}_{suffix}")
                    build(
                        compiler,
                        source,
                        binary,
                        standard=standard,
                        optimization=optimization,
                    )
                    repetitions = 10 if source.name == "mutex_concurrent.gti" else 1
                    for _ in range(repetitions):
                        execute([str(binary)])

        failure_source = root / "mutex_failure_cleanup.gti"
        failure_source.write_text(FAILURE_CLEANUP_SOURCE, encoding="utf-8")
        failure_binary = executable_path(root, "mutex_failure_cleanup")
        build(
            compiler,
            failure_source,
            failure_binary,
            standard="c++23",
            optimization="-O3",
        )
        result = execute([str(failure_binary)], expected=70, timeout=scaled(30))
        failure_line = source_line(FAILURE_CLEANUP_SOURCE, "int32_t failed =")
        expected_error = (
            f"mutex_failure_cleanup.gti:{failure_line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n"
        )
        if result.stdout != "waiter acquired\n" or result.stderr != expected_error:
            raise AssertionError(
                "guarded failure did not unlock before propagation\n"
                f"stdout: {result.stdout!r}\nstderr: {result.stderr!r}"
            )

    print("mutex native matrix and failure-cleanup checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
