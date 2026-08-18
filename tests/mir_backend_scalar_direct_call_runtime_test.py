#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


MARKER = "// GTI verified-MIR body: scalar-cfg-v1"
SELECTED = (
    "direct_identity",
    "selected_forward",
    "selected_order",
    "selected_void",
    "direct_chain_middle",
    "selected_chain",
    "direct_chain",
    "direct_unused",
    "direct_choose",
    "direct_nested",
    "direct_call_zero",
    "direct_literal",
    "direct_call_void",
    "direct_heterogeneous",
    "direct_loop",
    "direct_cross_namespace",
    "direct_identity_leaf",
    "direct_alternate_leaf",
    "direct_heterogeneous_leaf",
    "direct_first",
    "direct_pair",
    "direct_ping",
    "direct_sink",
    "direct_zero",
    "scoped_leaf",
    "is_valid",
    "get",
    # Native-calling prelude bodies admitted once the NativeInterop
    # capability row named the shipped gti_rt_* C-symbol surface.
    "write_stdout_byte",
    "read_stdin_byte",
    "read_file_byte",
    "close_file",
    # String-view signatures joined the boundary in 0.175.0: the C-linkage
    # call marshals the view through the shipped to_c_string_view converter.
    "write_stdout",
    "open_file_read",
    # The failure-free effect proof widened to passive views and native
    # calls in 0.177.0, so print's whole callee chain now proves and the
    # body emits from verified MIR.
    "print",
    # String-view literals joined the vocabulary in 0.178.0.
    "println",
    "print_decimal_digit",
    # Both print overloads carry markers; the second entry keeps the
    # name-derived count honest for the char overload admitted once
    # unchecked conversions joined the vocabulary (0.181.0).
    "print",
    # Callers the old whole-graph contract had to reject even though their
    # own bodies are ordinary: their ineligible callees stay on the
    # compatibility path while the calls emit from verified MIR.
    "compatibility_static_member",
    "compatibility_internal_call",
    "constexpr_target",
    "compatibility_constexpr_target",
    "compatibility_constexpr_call",
    "compatibility_for_target",
    "compatibility_for_call",
)
COMPATIBILITY = (
    "checked_target",
    "compatibility_checked_target",
    "compatibility_checked_call",
    "compatibility_internal_target",
    "compatibility_recursive",
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
    # The verified initializer bodies (the prelude's static marker, the
    # fixture classes', the text_view capability pair) and the empty
    # module body join the named function bodies.
    if (
        generated.count(f"{MARKER} function-instance") != len(SELECTED)
        # file_handle's computed negate(1) default joined the verified
        # initializer schedule as a terminally-contained expression.
        or generated.count(f"{MARKER} field-initializers-instance") != 3
        or generated.count(f"{MARKER} static-field-initializers-instance") != 3
        or generated.count(f"{MARKER} module-instance") != 1
    ):
        sys.stderr.write(
            f"{optimization}/{standard} did not select exactly the verified "
            "scalar MIR bodies plus the initializer and module markers\n"
        )
        return False
    for name in SELECTED:
        if MARKER not in function_definition(generated, name):
            sys.stderr.write(
                f"{optimization}/{standard} did not select {name} from MIR\n"
            )
            return False
    for name in COMPATIBILITY:
        if MARKER in function_definition(generated, name):
            sys.stderr.write(
                f"{optimization}/{standard} selected ineligible {name}\n"
            )
            return False
    if "::__gti_program::direct_support::" not in function_definition(
        generated, "direct_cross_namespace"
    ):
        sys.stderr.write(
            f"{optimization}/{standard} did not retain exact qualified "
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
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"direct-{optimization}-{standard}"
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

                emitted = root / f"direct-{optimization}-{standard}.cpp"
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
            o0_literal = function_definition(
                emitted_by_mode[("O0", standard)], "direct_literal"
            )
            o1_literal = function_definition(
                emitted_by_mode[("O1", standard)], "direct_literal"
            )
            o3_literal = function_definition(
                emitted_by_mode[("O3", standard)], "direct_literal"
            )
            if not o0_literal or o0_literal == o1_literal:
                sys.stderr.write(
                    f"O1/{standard} direct literal did not reflect its "
                    "verified identity-fold rewrite\n"
                )
                return 1
            if not o1_literal or o1_literal != o3_literal:
                sys.stderr.write(
                    f"O1/O3 {standard} direct literals did not preserve the "
                    "same verified identity fold\n"
                )
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
