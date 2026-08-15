#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


FUNCTION_MARKER = (
    "// GTI verified-MIR body: class-default-cleanup-v1 function-instance "
)
DESTRUCTOR_MARKER = (
    "// GTI verified-MIR body: class-default-cleanup-v1 destructor-instance "
)
COMPATIBILITY_FUNCTIONS = (
    "compatibility_declared_constructor",
    "compatibility_field_owner",
    "compatibility_nested_scope",
    "compatibility_branch",
    "compatibility_checked_cleanup",
)
COMPATIBILITY_DESTRUCTORS = (
    "ExplicitDefault",
    "FieldOwner",
    "CheckedCleanup",
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def definition_containing(generated: str, needle: str) -> str:
    position = generated.find(needle)
    while position != -1:
        line_end = generated.find("\n", position)
        opening = generated.find("{", position + len(needle))
        if opening != -1 and (line_end == -1 or opening < line_end):
            depth = 0
            for cursor in range(opening, len(generated)):
                if generated[cursor] == "{":
                    depth += 1
                elif generated[cursor] == "}":
                    depth -= 1
                    if depth == 0:
                        return generated[opening : cursor + 1]
        position = generated.find(needle, position + len(needle))
    return ""


def function_definition(generated: str, source_name: str) -> str:
    return definition_containing(generated, f"_{source_name}(")


def lifecycle_definition(generated: str, class_name: str) -> str:
    return definition_containing(
        generated, f"{class_name}::__gti_lifecycle_cleanup_"
    )


def selected_slot(body: str, class_name: str) -> str | None:
    match = re.search(
        rf"::gti_internal::backend::mir_lifetime_slot<[^>]*{class_name}>\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)",
        body,
    )
    return None if match is None else match.group(1)


def validate_family(generated: str, optimization: str, standard: str) -> bool:
    mode = f"{optimization}/{standard}"
    if generated.count(FUNCTION_MARKER) != 1:
        sys.stderr.write(f"{mode} did not select exactly one cleanup function\n")
        return False
    if generated.count(DESTRUCTOR_MARKER) != 2:
        sys.stderr.write(f"{mode} did not select exactly two cleanup destructors\n")
        return False

    selected = function_definition(generated, "selected_default_cleanup")
    if FUNCTION_MARKER not in selected:
        sys.stderr.write(f"{mode} did not emit selected_default_cleanup from MIR\n")
        return False
    if selected.count("::gti_internal::backend::mir_lifetime_slot<") != 2:
        sys.stderr.write(f"{mode} did not allocate exactly two strict MIR slots\n")
        return False
    if selected.count(".construct()") != 2 or selected.count(".destroy()") != 2:
        sys.stderr.write(
            f"{mode} did not emit exactly two explicit constructs and drops\n"
        )
        return False
    if (
        "std::optional<" in selected
        or "std::move(" in selected
        or "Early early" in selected
        or "Late late" in selected
    ):
        sys.stderr.write(f"{mode} used an unmodeled move or native RAII cleanup\n")
        return False

    early_slot = selected_slot(selected, "Early")
    late_slot = selected_slot(selected, "Late")
    early_drop = -1 if early_slot is None else selected.find(f"{early_slot}.destroy()")
    late_drop = -1 if late_slot is None else selected.find(f"{late_slot}.destroy()")
    if early_drop == -1 or late_drop == -1 or late_drop > early_drop:
        sys.stderr.write(f"{mode} did not preserve Late-before-Early MIR Drop order\n")
        return False

    for class_name in ("Early", "Late"):
        if DESTRUCTOR_MARKER not in lifecycle_definition(generated, class_name):
            sys.stderr.write(
                f"{mode} did not emit {class_name}'s lifecycle helper from MIR\n"
            )
            return False
    for name in COMPATIBILITY_FUNCTIONS:
        if FUNCTION_MARKER in function_definition(generated, name):
            sys.stderr.write(f"{mode} selected ineligible function {name}\n")
            return False
    for name in COMPATIBILITY_DESTRUCTORS:
        if DESTRUCTOR_MARKER in lifecycle_definition(generated, name):
            sys.stderr.write(f"{mode} selected ineligible destructor {name}\n")
            return False

    if "verified MIR lifetime slot escaped without Drop" not in generated:
        sys.stderr.write(f"{mode} omitted the strict MIR slot destructor guard\n")
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mir_backend_class_default_cleanup_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-mir-class-cleanup-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"cleanup-{optimization}-{standard}"
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

                emitted = root / f"cleanup-{optimization}-{standard}.cpp"
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
                generated = emitted.read_text(encoding="utf8")
                if not validate_family(generated, optimization, standard):
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
