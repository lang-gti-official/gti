#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import cpp_text_modes, is_cpp_mode, native_modes


MARKER = "// GTI verified-MIR body: scalar-cfg-v1"
FAILURE_MARKER = "// GTI verified-MIR body: scalar-cfg-failure-v1"
SELECTED = (
    "direct_identity",
    "selected_forward",
    "selected_order",
    "selected_void",
    "direct_chain_middle",
    "selected_chain",
    "direct_chain",
    "direct_choose",
    "direct_nested",
    "direct_call_zero",
    "direct_literal",
    "direct_call_void",
    "direct_heterogeneous",
    "direct_loop",
    "direct_cross_namespace",
    "direct_identity_leaf",
    "direct_heterogeneous_leaf",
    "direct_first",
    "direct_pair",
    "direct_ping",
    "direct_sink",
    "direct_zero",
    "scoped_leaf",
    # Callers the old whole-graph contract had to reject even though their
    # own bodies are ordinary now emit from verified MIR too.
    "pass",
    "compatibility_static_member",
    "compatibility_internal_target",
    "compatibility_internal_call",
    "constexpr_target",
    "compatibility_constexpr_target",
    "compatibility_constexpr_call",
    "compatibility_for_target",
    "compatibility_for_call",
)
FAILURE_SELECTED = (
    "checked_target",
    "compatibility_checked_target",
    "compatibility_checked_call",
    "checked_increment",
    "entry",
)
UNREACHABLE = ("direct_unused", "direct_alternate_leaf", "compatibility_recursive",
               "write_stdout_byte", "print", "println", "write_stdout", "print_decimal_digit")
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


def failure_function_definition(generated: str, source_name: str) -> str:
    return function_definition(generated, f"{source_name}__gti_mir_failure")


def validate_family(generated: str, level: str, label: str) -> bool:
    # Only StaticTarget's initializer bodies and the empty module remain structural
    # invariants; function coverage is checked by exact source name below so
    # unrelated prelude additions do not create count churn.
    if (
        generated.count(f"{MARKER} field-initializers-instance") != 1
        or generated.count(f"{MARKER} static-field-initializers-instance") != 1
        or generated.count(f"{MARKER} module-instance") != 1
    ):
        sys.stderr.write(
            f"{level}/{label} did not select exactly the verified "
            "scalar MIR bodies plus the initializer and module markers\n"
        )
        return False
    for name in UNREACHABLE:
        if function_definition(generated, name) or failure_function_definition(generated, name):
            sys.stderr.write(f"{level}/{label} emitted unreachable body {name}\n")
            return False
    for name in SELECTED:
        if MARKER not in function_definition(generated, name):
            sys.stderr.write(
                f"{level}/{label} did not select {name} from MIR\n"
            )
            return False
    for name in FAILURE_SELECTED:
        if FAILURE_MARKER not in failure_function_definition(generated, name):
            sys.stderr.write(
                f"{level}/{label} did not select {name} through "
                "the explicit failure-form MIR ABI\n"
            )
            return False
        if name not in ("entry", "print", "println") and function_definition(
            generated, name
        ):
            sys.stderr.write(
                f"{level}/{label} retained an ordinary wrapper "
                f"for failure-form function {name}\n"
            )
            return False
    if "::__gti_program::direct_support::" not in function_definition(
        generated, "direct_cross_namespace"
    ):
        sys.stderr.write(
            f"{level}/{label} did not retain exact qualified "
            "cross-namespace target identity\n"
        )
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mir_backend_scalar_direct_call_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-mir-scalar-direct-") as temporary:
        root = pathlib.Path(temporary)
        emitted_by_mode: dict[tuple[str, str], str] = {}
        for mode, flags, _native in native_modes():
            executable = root / f"direct-{mode}"
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

        # The C++ text probes: every level of the C++ profile, for the
        # per-level representation checks and the cross-level comparison.
        for label, flags, _native in cpp_text_modes():
            for level in ("O0", "O1", "O3"):
                emitted = root / f"direct-{label}-{level}.cpp"
                if is_cpp_mode(flags):
                    emission = run(
                        [
                            str(compiler),
                            str(source),
                            *flags,
                                f"-{level}",
                            "--emit-cpp",
                            "-o",
                            str(emitted),
                        ]
                    )
                    if emission.returncode != 0:
                        return fail(emission)
                    generated = emitted.read_text(encoding="utf8")
                    if not validate_family(generated, level, label):
                        return 1
                    emitted_by_mode[(level, label)] = generated
        for label, _flags, _native in cpp_text_modes():
            o0_literal = function_definition(
                emitted_by_mode[("O0", label)], "direct_literal"
            )
            o1_literal = function_definition(
                emitted_by_mode[("O1", label)], "direct_literal"
            )
            o3_literal = function_definition(
                emitted_by_mode[("O3", label)], "direct_literal"
            )
            if not o0_literal or o0_literal == o1_literal:
                sys.stderr.write(
                    f"O1/{label} direct literal did not reflect its "
                    "verified identity-fold rewrite\n"
                )
                return 1
            if not o1_literal or o1_literal != o3_literal:
                sys.stderr.write(
                    f"O1/O3 {label} direct literals did not preserve the "
                    "same verified identity fold\n"
                )
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
