#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile

from native_fixture_build import build_fixture


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def expect_rejected(
    compiler: pathlib.Path,
    source: pathlib.Path,
    diagnostic: str,
    output: pathlib.Path,
) -> bool:
    process = run([str(compiler), str(source), "-O0", "-o", str(output)])
    if (process.returncode == 0 or diagnostic not in process.stderr or
            "GTI-B0001" in process.stderr):
        fail(f"reject {source.name}", process)
        return False
    return True


def validate_representation(generated: str, mode: str) -> bool:
    declaration = re.search(r"struct optional_state__gti_enum_\w+\s*\{", generated)
    if declaration is None:
        sys.stderr.write(f"{mode}: missing source optional payload enum\n")
        return False
    # Inspect the concrete enum, not support templates emitted for other types.
    begin = declaration.end() - 1
    depth = 1
    end = begin + 1
    while end < len(generated) and depth:
        depth += (generated[end] == "{") - (generated[end] == "}")
        end += 1
    optional = generated[begin:end]
    required = (
        "struct __gti_variant_0",
        "struct __gti_variant_1",
        "::gti_internal::backend::payload_variant<std::monostate, "
        "__gti_variant_0, __gti_variant_1> __gti_value;",
    )
    forbidden = ("operator new", "unique_ptr", "std::optional<", "optional_storage")
    if depth or any(item not in optional for item in required) or any(
        item in optional for item in forbidden
    ):
        sys.stderr.write(f"{mode}: optional does not use inline generic enum storage\n")
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: optional_runtime_test.py <gti> <fixtures>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    optional_source = fixtures.parents[1] / "stdlib" / "std" / "optional.gti"
    read_line = next(
        index + 1 for index, line in enumerate(optional_source.read_text().splitlines())
        if "gti_internal::fail_empty_owner();" in line
    )
    with tempfile.TemporaryDirectory(prefix="gti-optional-") as temporary:
        root = pathlib.Path(temporary)
        if not expect_rejected(
            compiler,
            fixtures / "optional_copy_rejected.gti",
            "Cannot initialize 'copied'",
            root / "copy-rejected",
        ):
            return 1
        if not expect_rejected(
            compiler,
            fixtures / "optional_copy_assignment_rejected.gti",
            "error[GTI-S2003]: Cannot copy assign move-only type 'std::optional<int32_t>'",
            root / "assignment-rejected",
        ):
            return 1
        if not expect_rejected(
            compiler,
            fixtures / "optional_clone_rejected.gti",
            "error[GTI-S2029]: Generic arguments do not satisfy any alternative in concept 'std::cloneable'",
            root / "clone-rejected",
        ):
            return 1
        if not expect_rejected(
            compiler,
            fixtures / "optional_live_borrow_rejected.gti",
            "while a reference borrowed from it may still be live",
            root / "borrow-rejected",
        ):
            return 1

        for fixture_name in ("optional_borrowed_payload_rejected.gti",
                             "optional_borrowed_escape_rejected.gti"):
            if not expect_rejected(compiler, fixtures / fixture_name,
                                   "GTI-S2045", root / fixture_name):
                return 1

        if not expect_rejected(
            compiler, fixtures / "optional_borrowed_text_rejected.gti",
            "GTI-S2045", root / "borrowed-text-rejected",
        ):
            return 1

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                mode = f"{standard}/{optimization}"
                for fixture_name in ("optional_value.gti", "optional_lifecycle.gti",
                                     "optional_clone.gti", "optional_nested.gti",
                                     "optional_expected_composition.gti",
                                     "optional_move_view_local.gti",
                                     "optional_move_assignment.gti"):
                    executable = root / f"{fixture_name}-{standard}-{optimization}"
                    built = run(
                        [
                            str(compiler),
                            str(fixtures / fixture_name),
                            "--std",
                            standard,
                            f"-{optimization}",
                            "-o",
                            str(executable),
                        ]
                    )
                    if built.returncode != 0:
                        return fail(f"build {fixture_name} {mode}", built)
                    executed = run([str(executable)])
                    if executed.returncode != 0 or executed.stdout or executed.stderr:
                        return fail(f"run {fixture_name} {mode}", executed)

                allocated = root / f"allocations-{standard}-{optimization}"
                built = build_fixture(
                    [str(compiler), str(fixtures / "optional_allocations.gti"),
                     "--std", standard, f"-{optimization}"], allocated,
                    native_sources=[fixtures / "stdlib_adoption_allocation_counts.cpp"],
                    standard=standard, optimization=optimization,
                    text=True, capture_output=True, check=False, timeout=120)
                if built.returncode != 0:
                    return fail(f"build allocation probe {mode}", built)
                counted = run([str(allocated)])
                if counted.returncode or counted.stdout or counted.stderr:
                    return fail(f"run allocation probe {mode}", counted)

                for name, success, failure in (
                    ("optional_constructor_failure", "3\n12\n3\n9\n", "3\n3\n9\n"),
                    ("optional_reset_failure", "3\ndone\n9\n", "3\n9\n"),
                ):
                    source = fixtures / f"{name}.gti"
                    line = next(index + 1 for index, text in
                                enumerate(source.read_text().splitlines())
                                if "12 / divisor" in text)
                    binary = root / f"{name}-{standard}-{optimization}"
                    built = run([str(compiler), str(source), "--std", standard,
                                 f"-{optimization}", "-o", str(binary)])
                    if built.returncode:
                        return fail(f"build {name} {mode}", built)
                    for arguments, code, stdout, stderr in (
                        ([], 0, success, ""),
                        (["fail"], 70, failure,
                         f"{source.name}:{line}: runtime error[GTI-R0002]: "
                         "division_by_zero in integer_division\n"),
                    ):
                        executed = run([str(binary), *arguments])
                        if (executed.returncode, executed.stdout, executed.stderr) != (
                                code, stdout, stderr):
                            return fail(f"run {name} {mode} {arguments}", executed)

                generated = root / f"optional-{standard}-{optimization}.cpp"
                emission = run(
                    [
                        str(compiler),
                        str(fixtures / "optional_lifecycle.gti"),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "--emit-cpp",
                        "-o",
                        str(generated),
                    ]
                )
                if emission.returncode != 0:
                    return fail(f"emit {mode}", emission)
                if not validate_representation(
                    generated.read_text(encoding="utf8"), mode
                ):
                    return 1

                wrong_state = root / f"wrong-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(fixtures / "optional_wrong_state.gti"),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(wrong_state),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build wrong-state {mode}", built)
                executed = run([str(wrong_state)])
                expected_stderr = (
                    f"<std/optional>:{read_line}: runtime error[GTI-R0008]: "
                    "empty_owner_access in member_access\n"
                )
                if (
                    executed.returncode != 70
                    or executed.stdout != "73\n"
                    or executed.stderr != expected_stderr
                ):
                    return fail(f"run wrong-state {mode}", executed)
                print(f"source optional: {mode} passed", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
