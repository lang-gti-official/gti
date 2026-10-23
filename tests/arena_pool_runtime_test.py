#!/usr/bin/env python3

"""Exercise bounded source-defined arenas and pools through the GTI CLI."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


NEGATIVE_CASES = {
    "copy": (
        """\
#include <std/memory>
int main() {
  mut auto created = std::try_make_arena<int32_t>(std::size_t(1));
  if (!created) { return 1; }
  mut std::arena<int32_t> source = std::move(created).value();
  std::arena<int32_t> copied = source;
  return 0;
}
""",
        ("GTI-S2003", "Move-only owners cannot be copied"),
    ),
    "sealed-adoption": (
        """\
#include <std/memory>
int main() {
  gti_internal::empty_storage_adoption token =
      gti_internal::empty_storage_adoption::trusted;
  return 0;
}
""",
        ("GTI-S2058", "unavailable to application source"),
    ),
    "retained-borrow": (
        """\
#include <std/memory>
int main() {
  mut auto created = std::try_make_arena<int32_t>(std::size_t(1));
  if (!created) { return 1; }
  mut std::arena<int32_t> values = std::move(created).value();
  [[discard]] values.try_insert(int32_t(1));
  mut int32_t& borrowed = values.get_mut(std::size_t(0));
  values.reset();
  return borrowed;
}
""",
        ("GTI-S2017", "while a reference borrowed from it may still be live"),
    ),
    "nonmovable-element": (
        """\
#include <std/memory>
class pinned {
public:
  pinned() {}
  pinned(pinned& other) = delete;
  pinned(pinned&& other) = delete;
};
int main() {
  auto created = std::try_make_pool<pinned>(std::size_t(1));
  return 0;
}
""",
        (
            "GTI-S2029",
            "Type 'pinned' does not satisfy concept 'std::movable<pinned>'",
            "Required application 'std::movable<pinned>'",
        ),
    ),
}


FAILURE_CASES = {
    "arena-logical-bounds": (
        """\
#include <std/memory>
int main() {
  mut auto created = std::try_make_arena<int32_t>(std::size_t(2));
  if (!created) { return 1; }
  mut std::arena<int32_t> values = std::move(created).value();
  [[discard]] values.try_insert(int32_t(1));
  return values.get(std::size_t(1));
}
""",
        "",
        ("GTI-R0007", "index_out_of_bounds", "private_storage"),
    ),
    "pool-empty-slot": (
        """\
#include <std/memory>
int main() {
  mut auto created = std::try_make_pool<int32_t>(std::size_t(1));
  if (!created) { return 1; }
  mut std::pool<int32_t> values = std::move(created).value();
  return values.get(std::size_t(0));
}
""",
        "",
        ("GTI-R0010", "invalid_storage_state", "uninitialized_access"),
    ),
    "constructor-failure-cleanup": (
        """\
#include <std/memory>
class probe {
  int32_t stored;
public:
  probe(int32_t value) : stored(value) {}
  probe(probe& other) = delete;
  probe(probe&& other) = default;
  ~probe() { std::print("cleanup\\n"); }
};
int32_t fail_value() {
  return int32_t(2147483647) + int32_t(1);
}
int main() {
  mut auto created = std::try_make_arena<probe>(std::size_t(2));
  if (!created) { return 1; }
  mut std::arena<probe> values = std::move(created).value();
  [[discard]] values.try_insert(probe(int32_t(5)));
  [[discard]] values.try_insert(probe(fail_value()));
  return 2;
}
""",
        "cleanup\n",
        ("GTI-R0001", "integer_overflow", "addition"),
    ),
}


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
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(compiler),
            str(source),
            "--std",
            standard,
            f"-{optimization}",
            "-o",
            str(executable),
        ]
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: arena_pool_runtime_test.py <gti> <success-source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    success_source = pathlib.Path(sys.argv[2]).resolve()
    configurations = [
        (standard, optimization)
        for standard in ("c++20", "c++23")
        for optimization in ("O0", "O3")
    ]

    with tempfile.TemporaryDirectory(prefix="gti-arena-pool-") as temporary:
        root = pathlib.Path(temporary)
        for standard, optimization in configurations:
            executable = root / f"success-{standard}-{optimization}"
            built = build(
                compiler, success_source, executable, standard, optimization
            )
            if built.returncode != 0:
                return fail(f"build success {standard} {optimization}", built)
            executed = run([str(executable)])
            if executed.returncode != 0 or executed.stdout or executed.stderr:
                return fail(f"run success {standard} {optimization}", executed)

        for label, (source_text, fragments) in NEGATIVE_CASES.items():
            source = root / f"{label}.gti"
            source.write_text(source_text, encoding="utf-8")
            checked = run(
                [str(compiler), str(source), "--emit-cpp", "-o", str(root / "out.cpp")]
            )
            if checked.returncode != 65 or any(
                fragment not in checked.stderr for fragment in fragments
            ):
                return fail(f"check {label}", checked)

        for label, (source_text, stdout, fragments) in FAILURE_CASES.items():
            source = root / f"{label}.gti"
            source.write_text(source_text, encoding="utf-8")
            for standard, optimization in configurations:
                executable = root / f"{label}-{standard}-{optimization}"
                built = build(
                    compiler, source, executable, standard, optimization
                )
                if built.returncode != 0:
                    return fail(f"build {label} {standard} {optimization}", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 70
                    or executed.stdout != stdout
                    or any(fragment not in executed.stderr for fragment in fragments)
                ):
                    return fail(f"run {label} {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
