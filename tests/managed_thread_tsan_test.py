#!/usr/bin/env python3

"""Run managed-thread publication fixtures under ThreadSanitizer when available.

Every ``_tsan`` CTest sanitizes the LLVM backend's own emission of its fixture
and never C++-backend output. ``gti_llvm_instrumented_source``
(tests/llvm_instrumented_source.cpp) emits the publicly admitted verified
program as thread-sanitizer-attributed LLVM IR; the matching Clang compiles
that IR and the fixture's runtime dependency closure with ``-fsanitize=thread``;
the harness checks the object's instrumentation markers, links, and executes
the result repeatedly. ``atomic_tsan_test.py`` and ``mutex_tsan_test.py`` share
this recipe through import.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import SKIP_RETURN_CODE, scaled_timeout, thread_sanitizer_enabled

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent

# The complete runtime dependency closure of the sanitized fixtures. No
# uninstrumented GTI runtime archive or external provider is linked: every unit
# is compiled by the same native sanitizer toolchain as the fixture object.
RUNTIME_UNITS = ("execution", "failure", "thread", "cthread_state", "memory",
                 "atomic_wait", "mutex", "io")

# `nm -u` markers proving the generated object itself was instrumented rather
# than merely linked against the ThreadSanitizer runtime. The public atomic
# fixtures use mixed widths, so they require instrumented atomic operations of
# any width; the wrapping-atomic source owners keep their 64-bit pin.
INSTRUMENTED_MARKERS = ("__tsan_read", "__tsan_write")
ATOMIC_MARKERS = INSTRUMENTED_MARKERS + ("__tsan_atomic",)
WRAPPING_ATOMIC_MARKERS = INSTRUMENTED_MARKERS + ("__tsan_atomic64",)

# Fixtures whose accepted source owners use LLVM-only wrapping atomics. The
# frozen C++ backend rejects them before emitting anything; that rejection is
# pinned alongside the sanitized LLVM execution.
SOURCE_OWNER_FIXTURES = frozenset({
    "shared_mutex_threads", "transfer_function_threads",
    "shared_ownership_threads", "weak_ownership_threads",
    "shared_ownership_release_race",
})


def execute(
    command: list[str],
    *,
    environment: dict[str, str] | None = None,
    timeout: int | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=scaled_timeout(300) if timeout is None else timeout,
        check=False,
        env=environment,
    )


def thread_sanitizer_available(native_compiler: pathlib.Path, root: pathlib.Path) -> bool:
    """Whether the matching native compiler provides -fsanitize=thread on this host."""
    probe_source = root / "probe.cpp"
    probe_source.write_text("int main() { return 0; }\n", encoding="utf-8")
    probe = execute([str(native_compiler), "-std=c++20", "-fsanitize=thread",
                     str(probe_source), "-o", str(root / "probe")])
    return probe.returncode == 0


def compile_instrumented_runtime(runtime_compiler: pathlib.Path,
                                 root: pathlib.Path) -> list[str]:
    """Compile RUNTIME_UNITS once with the reviewed native compiler's thread sanitizer.

    runtime/src/atomic_lane.h admits only the reviewed compiler profile for
    the platform (GCC 14 on Linux x86_64, Apple clang 21 or Homebrew clang 23
    on macOS arm64), so the runtime is compiled by CMAKE_CXX_COMPILER, as in
    production, not by the LLVM-matching clang that compiles the emitted IR.
    """
    objects = []
    for unit in RUNTIME_UNITS:
        runtime_object = root / f"runtime-{unit}.o"
        compiled = execute([str(runtime_compiler), "-std=c++20", "-O1", "-g",
                            "-fsanitize=thread", "-fno-omit-frame-pointer",
                            "-fno-strict-aliasing", "-pthread",
                            "-I" + str(REPOSITORY / "runtime/include"),
                            "-c", str(REPOSITORY / "runtime/src" / (unit + ".cpp")),
                            "-o", str(runtime_object)])
        if compiled.returncode != 0:
            raise AssertionError(
                f"ThreadSanitizer runtime compile of {unit} returned {compiled.returncode}\n"
                f"stdout: {compiled.stdout}\nstderr: {compiled.stderr}")
        objects.append(str(runtime_object))
    return objects


def build_instrumented_binary(emitter: pathlib.Path, native_compiler: pathlib.Path,
                              source: pathlib.Path, binary: pathlib.Path,
                              runtime_objects: list[str], *,
                              markers: tuple[str, ...] = INSTRUMENTED_MARKERS,
                              linker: pathlib.Path | None = None) -> None:
    """Emit `source` through the LLVM backend with TSan attributes, then link it.

    The emitter writes `<binary>.ll`; the matching native compiler instruments
    it, and `nm -u` must show `markers` so an uninstrumented object cannot
    pass by linking to the sanitizer runtime alone. `linker` (the reviewed
    runtime compiler) links the object with the instrumented runtime units
    and its own thread-sanitizer runtime.
    """
    library = pathlib.Path(os.environ.get("GTI_STDLIB_PATH", REPOSITORY / "stdlib"))
    emitted = execute([str(emitter), str(library), str(source), str(binary), "O1",
                       str(native_compiler), "thread"])
    if emitted.returncode != 0:
        raise AssertionError(
            f"ThreadSanitizer LLVM emission of {source.name} returned {emitted.returncode}\n"
            f"stdout: {emitted.stdout}\nstderr: {emitted.stderr}")
    object_path = str(binary) + ".o"
    compiled = execute([str(native_compiler), "-O1", "-fsanitize=thread",
                        "-c", str(binary) + ".ll", "-o", object_path])
    if compiled.returncode != 0:
        raise AssertionError(
            f"ThreadSanitizer object compile of {source.name} returned {compiled.returncode}\n"
            f"stdout: {compiled.stdout}\nstderr: {compiled.stderr}")
    symbols = execute(["nm", "-u", object_path])
    assert (symbols.returncode == 0
            and all(marker in symbols.stdout for marker in markers)), (source.name, symbols)
    linked = execute([str(linker or native_compiler), "-fsanitize=thread", "-pthread",
                      object_path, *runtime_objects, "-o", str(binary)])
    if linked.returncode != 0:
        raise AssertionError(
            f"ThreadSanitizer link of {source.name} returned {linked.returncode}\n"
            f"stdout: {linked.stdout}\nstderr: {linked.stderr}")


def run_under_thread_sanitizer(binary: pathlib.Path, *, iterations: int = 3) -> None:
    environment = os.environ.copy()
    environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    for _ in range(iterations):
        run = execute([str(binary)], environment=environment)
        if run.returncode != 0:
            raise AssertionError(
                f"ThreadSanitizer run of {binary.name} returned {run.returncode}\n"
                f"stdout: {run.stdout}\nstderr: {run.stderr}")


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: managed_thread_tsan_test.py /path/to/gti fixture.gti "
            "/path/to/llvm-instrumented-source /path/to/matching-clang++ /path/to/reviewed-c++"
        )
    if not thread_sanitizer_enabled():
        print("managed thread ThreadSanitizer check skipped: unsupported runner")
        return SKIP_RETURN_CODE

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    emitter = pathlib.Path(sys.argv[3]).resolve()
    native_compiler = pathlib.Path(sys.argv[4]).absolute()
    runtime_compiler = pathlib.Path(sys.argv[5]).absolute()
    source_owner = source.stem in SOURCE_OWNER_FIXTURES

    with tempfile.TemporaryDirectory(prefix="gti-managed-thread-tsan-") as scratch:
        root = pathlib.Path(scratch)
        if not (thread_sanitizer_available(native_compiler, root)
                and thread_sanitizer_available(runtime_compiler, root)):
            print(
                "managed thread ThreadSanitizer check skipped: native compiler "
                "does not provide -fsanitize=thread"
            )
            return SKIP_RETURN_CODE

        if source_owner:
            rejected = execute([str(compiler), "--backend", "cpp",
                                "--execution-profile", "concurrent", "-O1",
                                str(source), "-o", str(root / "cpp-rejection")])
            assert (rejected.returncode == 65 and "GTI-B0003" in rejected.stderr
                    and "wrapping atomic read-modify-write" in rejected.stderr), rejected

        binary = root / source.stem
        runtime_objects = compile_instrumented_runtime(runtime_compiler, root)
        build_instrumented_binary(
            emitter, native_compiler, source, binary, runtime_objects,
            markers=WRAPPING_ATOMIC_MARKERS if source_owner else INSTRUMENTED_MARKERS,
            linker=runtime_compiler)
        run_under_thread_sanitizer(binary)

    print("managed thread ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
