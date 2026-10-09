#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


MARKER = "// GTI verified-MIR body: scalar-cfg-v1"
FAILURE_MARKER = "// GTI verified-MIR body: scalar-cfg-failure-v1"
SELECTED = (
    "cfg_not",
    "cfg_char",
    "cfg_bits",
    "cfg_less",
    "cfg_choose",
    "cfg_local",
    "cfg_switch",
    "cfg_short",
    "cfg_loop",
    "cfg_fold",
    "cfg_grouped_local",
    "cfg_comma_local",
    "cfg_grouped_condition",
)
FAILURE_SELECTED = (
    "compatibility_checked",
    "compatibility_call_target",
    "compatibility_call",
    "compatibility_reference",
    "entry",
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def function_definition(generated: str, source_name: str) -> str:
    needle = f"_{source_name}("
    position = generated.find(needle)
    while position != -1:
        line_end = generated.find("\n", position)
        brace = generated.find(" {\n", position)
        if brace != -1 and (line_end == -1 or brace < line_end):
            end = generated.find("\n  }", brace)
            return "" if end == -1 else generated[brace : end + 4]
        position = generated.find(needle, position + len(needle))
    return ""


def validate_family(generated: str, optimization: str, standard: str) -> bool:
    # Only reachable function bodies and the module are lowered. The fixture
    # does not use the prelude text-view classes or its output functions.
    if (
        generated.count(f"{MARKER} function-instance")
        != len(SELECTED)
        or generated.count(f"{FAILURE_MARKER} function-instance") != len(FAILURE_SELECTED)
        or generated.count(f"{MARKER} field-initializers-instance") != 0
        or generated.count(f"{MARKER} static-field-initializers-instance") != 0
        or generated.count(f"{MARKER} module-instance") != 1
    ):
        sys.stderr.write(
            f"{optimization}/{standard} did not select exactly the verified "
            "scalar CFG MIR bodies plus the initializer and module markers\n"
        )
        return False
    if (
        "switch (__gti_mir_bb)" in generated
        or "std::size_t __gti_mir_bb" in generated
        or "__gti_mir_bb =" in generated
        or "goto __gti_mir_bb_" not in generated
        or "__gti_mir_bb_1: {" not in generated
    ):
        sys.stderr.write(
            f"{optimization}/{standard} did not use the complete direct-label "
            "MIR CFG representation\n"
        )
        return False
    for name in SELECTED:
        if MARKER not in function_definition(generated, name):
            sys.stderr.write(
                f"{optimization}/{standard} did not select {name} from MIR\n"
            )
            return False
    for name in FAILURE_SELECTED:
        if FAILURE_MARKER not in function_definition(
            generated, f"{name}__gti_mir_failure"
        ):
            sys.stderr.write(
                f"{optimization}/{standard} did not select {name} through "
                "the closed failure-form MIR component\n"
            )
            return False
    if (
        "_print__gti_mir_failure(" in generated
        or "_println__gti_mir_failure(" in generated
        or f"{FAILURE_MARKER} function-instance" not in function_definition(
            generated, "entry__gti_mir_failure"
        )
    ):
        sys.stderr.write(
            f"{optimization}/{standard} emitted unused output bodies or "
            "lost the hosted-entry body\n"
        )
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mir_backend_scalar_cfg_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-mir-scalar-cfg-") as temporary:
        root = pathlib.Path(temporary)
        emitted_by_mode: dict[tuple[str, str], str] = {}
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"scalar-cfg-{optimization}-{standard}"
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

                emitted = root / f"scalar-cfg-{optimization}-{standard}.cpp"
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
                emitted_by_mode[(optimization, standard)] = generated

        for standard in ("c++20", "c++23"):
            o0_fold = function_definition(
                emitted_by_mode[("O0", standard)], "cfg_fold"
            )
            o1_fold = function_definition(
                emitted_by_mode[("O1", standard)], "cfg_fold"
            )
            o3_fold = function_definition(
                emitted_by_mode[("O3", standard)], "cfg_fold"
            )
            if not o0_fold or o0_fold == o1_fold:
                sys.stderr.write(
                    f"marked O1/{standard} CFG did not reflect its verified "
                    "identity-fold rewrite\n"
                )
                return 1
            if not o1_fold or o1_fold != o3_fold:
                sys.stderr.write(
                    f"marked O1/O3 {standard} CFG bodies did not preserve the "
                    "same verified identity fold\n"
                )
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
