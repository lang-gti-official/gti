#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


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
    begin = generated.find("template <typename T>\nclass optional_storage {")
    end = generated.find("// Prefix-initialized storage", begin)
    if begin < 0 or end < 0:
        sys.stderr.write(f"{mode}: missing optional storage representation\n")
        return False
    optional = generated[begin:end]
    required = (
        "alignas(T) std::byte storage_[sizeof(T)]",
        "bool engaged_ = false",
        "construct_representation_at(slot(),",
        "std::destroy_at",
        "optional_storage(optional_storage &&other)",
    )
    forbidden = ("operator new", "unique_ptr", "std::optional<")
    if any(item not in optional for item in required) or any(
        item in optional for item in forbidden
    ):
        sys.stderr.write(f"{mode}: optional storage is not the inline checked slot\n")
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
        if "return gti_internal::optional_storage_read(this.storage);" in line
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
            fixtures / "optional_move_assignment_rejected.gti",
            "error[GTI-S2003]: Move assignment is unavailable for type 'std::optional<int32_t>'",
            root / "move-assignment-rejected",
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
                                   "GTI-S2019", root / fixture_name):
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
                                     "optional_move_view_local.gti"):
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
