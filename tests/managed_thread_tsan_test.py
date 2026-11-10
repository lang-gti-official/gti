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

from backend_modes import cpp_modes
from sanitizer_support import SKIP_RETURN_CODE, scaled_timeout, thread_sanitizer_enabled

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent


# `nm -u` markers proving the generated object itself was instrumented rather
# than merely linked against the ThreadSanitizer runtime. The public atomic
# fixtures use mixed widths, so they require instrumented atomic operations of
# any width; the wrapping-atomic source owners keep their 64-bit pin.
INSTRUMENTED_MARKERS = ("__tsan_read", "__tsan_write")
ATOMIC_MARKERS = INSTRUMENTED_MARKERS + ("__tsan_atomic",)
WRAPPING_ATOMIC_MARKERS = INSTRUMENTED_MARKERS + ("__tsan_atomic64",)

# Fixtures whose accepted source owners use LLVM-only wrapping atomics. The
# frozen C++ backend rejects them before emitting anything; that rejection is
# pinned alongside the sanitized LLVM execution when GTI_TEST_BACKENDS selects
# the C++ backend.
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


def global_symbols(path: pathlib.Path) -> list[tuple[str, str]]:
    """``(name, nm type)`` for every external symbol of the object `path`."""
    listed = execute(["nm", "-g", "-P", str(path)])
    if listed.returncode != 0:
        raise AssertionError(f"nm of {path.name} returned {listed.returncode}\n"
                             f"stdout: {listed.stdout}\nstderr: {listed.stderr}")
    symbols = []
    for line in listed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and len(fields[1]) == 1:
            symbols.append((fields[0], fields[1]))
    return symbols


class InstrumentedRuntime:
    """The runtime units a fixture links, each compiled with the thread sanitizer.

    No uninstrumented GTI runtime archive or external provider is linked:
    every unit is compiled by the same native sanitizer toolchain as the
    fixture object. The unit set is not a hand-written list. It is the set of
    members that a static link of the fixture object would pull from the
    production runtime archive (``$<TARGET_FILE:gti_runtime>``, exported to
    the test as ``GTI_RUNTIME_LIBRARY``), which is built from the same source
    list as every ordinary program's runtime. A runtime entry point added in a
    new source therefore joins the sanitized link with that source.

    runtime/src/atomic_lane.h admits only the reviewed compiler profile for
    the platform (GCC 14 on Linux x86_64, Apple clang 21 or Homebrew clang 23
    on macOS arm64), so the runtime is compiled by CMAKE_CXX_COMPILER, as in
    production, not by the LLVM-matching clang that compiles the emitted IR.
    """

    def __init__(self, compiler: pathlib.Path, root: pathlib.Path,
                 archive: pathlib.Path) -> None:
        self.compiler = compiler
        self.root = root
        members_directory = root / "runtime-archive"
        members_directory.mkdir()
        listed = execute(["ar", "t", str(archive)])
        # `ar x` extracts into its working directory.
        extracted = subprocess.run(["ar", "x", str(archive)], cwd=members_directory,
                                   capture_output=True, text=True, check=False,
                                   timeout=scaled_timeout(60))
        if listed.returncode != 0 or extracted.returncode != 0:
            raise AssertionError(f"cannot read the runtime archive {archive}\n"
                                 f"stderr: {listed.stderr}{extracted.stderr}")
        # Archive order, as a static link searches it.
        self.members = [name for name in listed.stdout.splitlines()
                        if name.endswith((".o", ".obj"))]
        if len(set(self.members)) != len(self.members):
            raise AssertionError(f"runtime archive {archive} repeats a member name")
        self.undefined: dict[str, set[str]] = {}
        strong: dict[str, str] = {}
        weak: dict[str, str] = {}
        for member in self.members:
            self.undefined[member] = set()
            for name, kind in global_symbols(members_directory / member):
                if kind == "U":
                    self.undefined[member].add(name)
                elif kind in "WV":
                    weak.setdefault(name, member)
                elif kind not in "wvI":
                    strong.setdefault(name, member)
        # A strong definition satisfies a reference before a weak one does.
        self.provider = {**weak, **strong}
        self.objects: dict[str, str] = {}

    def source(self, member: str) -> pathlib.Path:
        unit = member.removesuffix(".obj").removesuffix(".o")
        source = REPOSITORY / "runtime/src" / (unit if unit.endswith(".cpp") else unit + ".cpp")
        if not source.is_file():
            raise AssertionError(f"runtime archive member {member} has no runtime/src source")
        return source

    def compile(self, member: str) -> str:
        if member not in self.objects:
            source = self.source(member)
            runtime_object = self.root / f"runtime-{source.stem}.o"
            compiled = execute([str(self.compiler), "-std=c++20", "-O1", "-g",
                                "-fsanitize=thread", "-fno-omit-frame-pointer",
                                "-fno-strict-aliasing", "-pthread",
                                "-I" + str(REPOSITORY / "runtime/include"),
                                "-c", str(source), "-o", str(runtime_object)])
            if compiled.returncode != 0:
                raise AssertionError(
                    f"ThreadSanitizer runtime compile of {source.stem} returned "
                    f"{compiled.returncode}\nstdout: {compiled.stdout}\nstderr: {compiled.stderr}")
            self.objects[member] = str(runtime_object)
        return self.objects[member]

    def closure(self, object_path: pathlib.Path) -> list[str]:
        """Archive members a static link of `object_path` pulls in, in archive order."""
        pending = [name for name, kind in global_symbols(object_path) if kind == "U"]
        selected: set[str] = set()
        while pending:
            member = self.provider.get(pending.pop())
            if member is not None and member not in selected:
                selected.add(member)
                pending.extend(self.undefined[member])
        return [member for member in self.members if member in selected]

    def objects_for(self, object_path: pathlib.Path) -> list[str]:
        return [self.compile(member) for member in self.closure(object_path)]


def compile_instrumented_runtime(runtime_compiler: pathlib.Path,
                                 root: pathlib.Path) -> InstrumentedRuntime:
    """The instrumented runtime of GTI_RUNTIME_LIBRARY's archive, compiled on demand."""
    archive = os.environ.get("GTI_RUNTIME_LIBRARY")
    if not archive or not pathlib.Path(archive).is_file():
        raise AssertionError("GTI_RUNTIME_LIBRARY must name the built gti_runtime archive "
                             "whose dependency closure the sanitized link compiles")
    return InstrumentedRuntime(runtime_compiler, root, pathlib.Path(archive))


def build_instrumented_binary(emitter: pathlib.Path, native_compiler: pathlib.Path,
                              source: pathlib.Path, binary: pathlib.Path,
                              runtime: InstrumentedRuntime, *,
                              markers: tuple[str, ...] = INSTRUMENTED_MARKERS,
                              linker: pathlib.Path | None = None) -> None:
    """Emit `source` through the LLVM backend with TSan attributes, then link it.

    The emitter writes `<binary>.ll`; the matching native compiler instruments
    it, and `nm -u` must show `markers` so an uninstrumented object cannot
    pass by linking to the sanitizer runtime alone. `linker` (the reviewed
    runtime compiler) links the object with the instrumented runtime units it
    depends on and its own thread-sanitizer runtime.
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
                      object_path, *runtime.objects_for(pathlib.Path(object_path)),
                      "-o", str(binary)])
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

        if source_owner and cpp_modes():
            rejected = execute([str(compiler), "--backend", "cpp",
                                "--execution-profile", "concurrent", "-O1",
                                str(source), "-o", str(root / "cpp-rejection")])
            assert (rejected.returncode == 65 and "GTI-B0003" in rejected.stderr
                    and "wrapping atomic read-modify-write" in rejected.stderr), rejected

        binary = root / source.stem
        runtime = compile_instrumented_runtime(runtime_compiler, root)
        build_instrumented_binary(
            emitter, native_compiler, source, binary, runtime,
            markers=WRAPPING_ATOMIC_MARKERS if source_owner else INSTRUMENTED_MARKERS,
            linker=runtime_compiler)
        run_under_thread_sanitizer(binary)

    print("managed thread ThreadSanitizer check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
