#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def validate_emission(generated: str, mode: str) -> bool:
    required = (
        "StructuralValue() = default;",
        "~StructuralValue() noexcept = default;",
        "StructuralPair() = default;",
        "~StructuralPair() noexcept = default;",
        "StructuralBase() = default;",
        "~StructuralBase() noexcept = default;",
        "StructuralDerived() = default;",
        "~StructuralDerived() noexcept = default;",
        "mir_lifetime_slot<::__gti_program::StructuralValue>",
        "mir_lifetime_slot<::__gti_program::StructuralPair>",
        "mir_lifetime_slot<::__gti_program::StructuralDerived>",
        ".construct()",
        ".destroy()",
    )
    forbidden = (
        "StructuralValue::StructuralValue(",
        "StructuralValue::~StructuralValue(",
        "StructuralPair::StructuralPair(",
        "StructuralPair::~StructuralPair(",
        "StructuralBase::StructuralBase(",
        "StructuralBase::~StructuralBase(",
        "StructuralDerived::StructuralDerived(",
        "StructuralDerived::~StructuralDerived(",
        "destructor-instance ",
    )
    if any(item not in generated for item in required):
        sys.stderr.write(
            f"{mode} did not retain defaulted structural lifecycle emission\n"
        )
        return False
    if any(item in generated for item in forbidden):
        sys.stderr.write(f"{mode} emitted a source lifecycle body for '= default'\n")
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
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}/{standard}"
                executable = root / f"defaulted-{optimization}-{standard}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)
                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(executed)

                emitted = root / f"defaulted-{optimization}-{standard}.cpp"
                emission = run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "--emit-cpp",
                        "-o",
                        str(emitted),
                    ]
                )
                if emission.returncode != 0:
                    return fail(emission)
                if not validate_emission(emitted.read_text(encoding="utf8"), mode):
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
