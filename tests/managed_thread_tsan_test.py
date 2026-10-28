#!/usr/bin/env python3

"""Run managed class-result publication under ThreadSanitizer when available."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import SKIP_RETURN_CODE, thread_sanitizer_enabled


def execute(
    command: list[str],
    *,
    environment: dict[str, str] | None = None,
    timeout: int = 300,
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
    if len(sys.argv) not in (4, 6):
        raise SystemExit(
            "usage: managed_thread_tsan_test.py /path/to/gti result.gti "
            "/path/to/c++ [/path/to/llvm-instrumented-source /path/to/matching-clang++]"
        )
    if not thread_sanitizer_enabled():
        print("managed thread ThreadSanitizer check skipped: unsupported runner")
        return SKIP_RETURN_CODE

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    native_compiler = pathlib.Path(sys.argv[3]).absolute()
    source_owner = source.stem in {"shared_mutex_threads", "transfer_function_threads"}
    if source_owner:
        assert len(sys.argv) == 6, "LLVM helper and matching native compiler are required"
        native_compiler = pathlib.Path(sys.argv[5]).absolute()

    with tempfile.TemporaryDirectory(prefix="gti-managed-thread-tsan-") as scratch:
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
                "managed thread ThreadSanitizer check skipped: native compiler "
                "does not provide -fsanitize=thread"
            )
            return SKIP_RETURN_CODE

        binary = root / "managed_thread_class_result"
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
        if source_owner:
            # The accepted source owners use LLVM-only wrapping atomics. Pin
            # that C++ rejection, then instrument the actual generated LLVM
            # object; linking an uninstrumented object to TSan is insufficient.
            assert (build.returncode == 65 and "GTI-B0003" in build.stderr
                    and "wrapping atomic read-modify-write" in build.stderr), build
            repository = pathlib.Path(__file__).resolve().parent.parent
            library = pathlib.Path(os.environ.get("GTI_STDLIB_PATH", repository / "stdlib"))
            build = execute([str(pathlib.Path(sys.argv[4]).resolve()), str(library),
                             str(source), str(binary), "O1", str(native_compiler), "thread"])
            if build.returncode == 0:
                build = execute([str(native_compiler), "-O1", "-fsanitize=thread",
                                 "-c", str(binary) + ".ll", "-o", str(binary) + ".o"])
                assert build.returncode == 0, build
                symbols = execute(["nm", "-u", str(binary) + ".o"])
                assert (symbols.returncode == 0 and all(marker in symbols.stdout for marker in
                        ("__tsan_read", "__tsan_write", "__tsan_atomic64"))), symbols
                # This fixture links no uninstrumented GTI runtime archive or
                # external provider. Compile the complete runtime dependency
                # closure explicitly with the same native sanitizer toolchain.
                objects = [str(binary) + ".o"]
                for unit in ("execution", "failure", "thread", "cthread_state",
                             "memory", "atomic_wait", "mutex"):
                    runtime_object = root / f"runtime-{unit}.o"
                    compiled = execute([str(native_compiler), "-std=c++20", "-O1", "-g",
                                        "-fsanitize=thread", "-fno-omit-frame-pointer",
                                        "-fno-strict-aliasing", "-pthread",
                                        "-I" + str(repository / "runtime/include"),
                                        "-c", str(repository / "runtime/src" / (unit + ".cpp")),
                                        "-o", str(runtime_object)])
                    assert compiled.returncode == 0, compiled
                    objects.append(str(runtime_object))
                build = execute([str(native_compiler), "-fsanitize=thread", "-pthread",
                                 *objects, "-o", str(binary)])
        if build.returncode != 0:
            raise AssertionError(
                f"ThreadSanitizer build returned {build.returncode}\n"
                f"stdout: {build.stdout}\nstderr: {build.stderr}"
            )

        environment = os.environ.copy()
        environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        for _ in range(3):
            run = execute([str(binary)], environment=environment)
            if run.returncode != 0:
                raise AssertionError(
                    f"ThreadSanitizer run returned {run.returncode}\n"
                    f"stdout: {run.stdout}\nstderr: {run.stderr}"
                )

    print("managed thread ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
