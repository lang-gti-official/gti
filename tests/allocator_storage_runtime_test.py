#!/usr/bin/env python3

"""Exercise the public typed allocator/storage slice through the GTI CLI."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from native_symbol_spelling import native_spelling


FAILURE_CASES = {
    "bounds": (
        """\
import std.memory;

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  [[discard]] values.initialized(std::size_t(1));
  return 2;
}
""",
        ("GTI-R0007", "index_out_of_bounds", "private_storage", "<std/memory>"),
    ),
    "duplicate-construction": (
        """\
import std.memory;

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  values.construct(std::size_t(0), int32_t(1));
  values.construct(std::size_t(0), int32_t(2));
  return 2;
}
""",
        ("GTI-R0010", "invalid_storage_state", "duplicate_construction",
         "<std/detail/memory_storage>"),
    ),
    "uninitialized-access": (
        """\
import std.memory;

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  return values.get(std::size_t(0));
}
""",
        ("GTI-R0010", "invalid_storage_state", "uninitialized_access",
         "<std/detail/memory_storage>"),
    ),
    "uninitialized-destruction": (
        """\
import std.memory;

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  values.destroy(std::size_t(0));
  return 2;
}
""",
        ("GTI-R0010", "invalid_storage_state", "uninitialized_access",
         "<std/detail/memory_storage>"),
    ),
}


CONSTRUCTOR_FAILURE_CLEANUP = """\
import std.memory;

class probe {
  int32_t value;

public:
  probe(int32_t input) : value(input) {}

  probe(probe& other) = delete;
  probe(probe&& other) = default;

  ~probe() {
    std::print("cleanup\\n");
  }
};

int32_t fail_value() {
  return int32_t(2147483647) + int32_t(1);
}

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<probe>(std::size_t(2));
  if (!allocated) { return 1; }
  mut std::storage<probe> values = std::move(allocated).value();
  values.construct(std::size_t(0), probe(int32_t(5)));
  values.construct(std::size_t(1), probe(fail_value()));
  return 2;
}
"""


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def build(
    compiler: pathlib.Path,
    source: pathlib.Path,
    executable: pathlib.Path,
    standard: str,
    optimization: str,
    native_source: pathlib.Path | None = None,
    keep_cpp: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = [
        str(compiler),
        str(source),
        "--std",
        standard,
        f"-{optimization}",
        "-o",
        str(executable),
    ]
    if keep_cpp:
        command.append("--keep-cpp")
    if native_source is not None:
        command.extend(["--", str(native_source)])
    return run(command)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: allocator_storage_runtime_test.py "
            "<gti> <success-source> <alignment-native-source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    success_source = pathlib.Path(sys.argv[2]).resolve()
    alignment_source = pathlib.Path(sys.argv[3]).resolve()
    configurations = [
        (standard, optimization)
        for standard in ("c++20", "c++23")
        for optimization in ("O0", "O3")
    ]

    with tempfile.TemporaryDirectory(prefix="gti-allocator-storage-") as temporary:
        root = pathlib.Path(temporary)
        for standard, optimization in configurations:
            executable = root / f"success-{standard}-{optimization}"
            keep_cpp = standard == "c++20" and optimization == "O0"
            built = build(
                compiler,
                success_source,
                executable,
                standard,
                optimization,
                alignment_source,
                keep_cpp,
            )
            if built.returncode != 0:
                return fail(f"build success {standard} {optimization}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != ""
                or executed.stderr != ""
            ):
                return fail(f"run success {standard} {optimization}", executed)

            if keep_cpp:
                generated = pathlib.Path(f"{executable}.gti.cpp").read_text(
                    encoding="utf-8"
                )
                required = (
                    f"{native_spelling(generated, 'gti_rt_native_size_max_v1')}(",
                    f"{native_spelling(generated, 'gti_rt_address_is_aligned_v1')}(",
                    "::gti_internal::backend::native_pointer_value(",
                )
                missing = [fragment for fragment in required if fragment not in generated]
                retired = [fragment for fragment in (
                    "::gti_internal::backend::storage_try_allocate(",
                    "::gti_internal::backend::storage_capacity(",
                    "::gti_internal::backend::storage_initialized(",
                ) if fragment in generated]
                if missing or retired:
                    sys.stderr.write(
                        "generated allocator/storage boundary is incomplete: "
                        f"missing={missing}, retired={retired}\n"
                    )
                    return 1

        for label, (source_text, fragments) in FAILURE_CASES.items():
            source = root / f"{label}.gti"
            source.write_text(source_text, encoding="utf-8")
            for standard, optimization in configurations:
                executable = root / f"{label}-{standard}-{optimization}"
                built = build(
                    compiler, source, executable, standard, optimization
                )
                if built.returncode != 0:
                    return fail(
                        f"build {label} {standard} {optimization}", built
                    )
                executed = run([str(executable)])
                if (
                    executed.returncode != 70
                    or executed.stdout != ""
                    or any(fragment not in executed.stderr for fragment in fragments)
                ):
                    return fail(
                        f"run {label} {standard} {optimization}", executed
                    )

        source = root / "constructor-failure-cleanup.gti"
        source.write_text(CONSTRUCTOR_FAILURE_CLEANUP, encoding="utf-8")
        for standard, optimization in configurations:
            executable = root / f"constructor-failure-cleanup-{standard}-{optimization}"
            built = build(compiler, source, executable, standard, optimization)
            if built.returncode != 0:
                return fail(
                    f"build constructor-failure-cleanup {standard} {optimization}",
                    built,
                )
            executed = run([str(executable)])
            if (
                executed.returncode != 70
                or executed.stdout != "cleanup\n"
                or "GTI-R0001" not in executed.stderr
                or "integer_overflow" not in executed.stderr
                or "addition" not in executed.stderr
                or "constructor-failure-cleanup.gti" not in executed.stderr
            ):
                return fail(
                    f"run constructor-failure-cleanup {standard} {optimization}",
                    executed,
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
