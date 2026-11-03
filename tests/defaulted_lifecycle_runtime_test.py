#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def validate_emission(generated: str, mode: str) -> bool:
    required = (
        "~StructuralValue() noexcept = default;",
        "~StructuralPair() noexcept = default;",
        "~StructuralBase() noexcept = default;",
        "~StructuralDerived() noexcept = default;",
        "mir_lifetime_slot<::__gti_program::StructuralValue>",
        "mir_lifetime_slot<::__gti_program::StructuralPair>",
        "mir_lifetime_slot<::__gti_program::StructuralDerived>",
        ".construct()",
        ".construction_address()",
        ".mark_constructed()",
        ".destroy()",
    )
    forbidden = (
        "StructuralValue::~StructuralValue(",
        "StructuralPair::~StructuralPair(",
        "StructuralBase::~StructuralBase(",
        "StructuralDerived::~StructuralDerived(",
        "destructor-instance ",
    )
    if any(item not in generated for item in required):
        sys.stderr.write(
            f"{mode} did not retain defaulted structural lifecycle emission\n"
        )
        return False
    if any(item in generated for item in forbidden):
        sys.stderr.write(f"{mode} emitted a source destructor body for '= default'\n")
        return False
    # '= default' uses generated GTI constructor schedules for declaration
    # defaults and base ordering; it does not delegate those semantics to a
    # native '= default'. Trivial copy/move/destruction stay structural.
    for name in ("StructuralValue", "StructuralPair", "StructuralBase", "StructuralDerived"):
        constructor = re.compile(
            rf"^  (?:__gti_small_source_inline )?{name}::{name}\([^\n]*\n"
            r"    // GTI verified-MIR body: scalar-cfg-(?:constructor-failure-)?v1 constructor-instance \d+",
            re.MULTILINE,
        )
        if (
            not constructor.search(generated)
            or f"{name}() = default;" in generated
            or f"{name}(const {name} &other) = default;" not in generated
            or f"{name}({name} &&) = default;" not in generated
        ):
            sys.stderr.write(f"{mode} lost {name}'s generated constructor or structural transfer\n")
            return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: defaulted_lifecycle_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-defaulted-lifecycle-") as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, _native in native_modes():
            executable = root / f"defaulted-{mode}"
            built = run(
                [
                    str(compiler),
                    str(source),
                    *flags,
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                return fail(built)
            executed = run([str(executable)])
            if executed.returncode != 0:
                return fail(executed)

            emitted = root / f"defaulted-{mode}.cpp"
            if is_cpp_mode(flags):
                emission = run(
                    [
                        str(compiler),
                        str(source),
                        *flags,
                        "--emit-cpp",
                        "-o",
                        str(emitted),
                    ]
                )
                if emission.returncode != 0:
                    return fail(emission)
            if is_cpp_mode(flags):
                if not validate_emission(emitted.read_text(encoding="utf8"), mode):
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
