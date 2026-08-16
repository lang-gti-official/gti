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


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mir_backend_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    marker = "// GTI verified-MIR body: scalar-cfg-v1"
    with tempfile.TemporaryDirectory(prefix="gti-mir-backend-") as temporary:
        root = pathlib.Path(temporary)
        emitted_by_mode: dict[tuple[str, str], str] = {}
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"mir-backend-{optimization}-{standard}"
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

                emitted = root / f"mir-backend-{optimization}-{standard}.cpp"
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
                if generated.count(marker) != 21:
                    sys.stderr.write(
                        "generated C++ did not select exactly the twenty-one "
                        "verified scalar MIR bodies (twelve fixture bodies "
                        "plus nine prelude bodies)\n"
                    )
                    return 1
                checked = function_definition(generated, "compatibility_checked")
                if (
                    "__gti_mir_failure(" not in checked
                    or "gti_rt_failure_terminate_v1" not in checked
                    or generated.count(
                        "// GTI verified-MIR body: scalar-cfg-failure-v1"
                    )
                    != 4
                ):
                    sys.stderr.write(
                        "the four failure-form bodies should emit their "
                        "transformed bodies plus boundary wrappers (ADR 017: "
                        "compatibility_checked, checked_leaf, checked_caller, "
                        "and the prelude's file_handle release member)\n"
                    )
                    return 1
                if (
                    "checked_leaf__gti_mir_failure(" not in generated
                    or "__gti_mir_call_success_" not in generated
                ):
                    sys.stderr.write(
                        "checked_caller should reach checked_leaf through "
                        "the transformed convention: a direct derived-name "
                        "call branching on the call-success bool\n"
                    )
                    return 1
                if (
                    "std::int8_t"
                    not in function_definition(generated, "mir_i8_identity")
                    or "mir_second(std::int8_t __gti_mir_arg_0, std::int32_t "
                    "__gti_mir_arg_1, std::uint64_t __gti_mir_arg_2)"
                    not in generated
                    or "= __gti_mir_arg_1;"
                    not in function_definition(generated, "mir_second")
                    or "static_cast<std::int64_t>(9223372036854775807)"
                    not in function_definition(generated, "mir_i64_max")
                    or "18446744073709551615ULL"
                    not in function_definition(generated, "mir_u64_max")
                    or "return;"
                    not in function_definition(generated, "mir_noop")
                ):
                    sys.stderr.write(
                        "generated MIR family lost fixed-width or void semantics\n"
                    )
                    return 1
                if (
                    marker
                    not in function_definition(
                        generated, "compatibility_bool_identity"
                    )
                    or marker
                    not in function_definition(
                        generated, "compatibility_char_identity"
                    )
                ):
                    sys.stderr.write(
                        "bool or character identity bodies should now be "
                        "admitted per body by the general emitter\n"
                    )
                    return 1

                constant = function_definition(generated, "mir_constant")
                if optimization == "O0":
                    exact_transform = (
                        "__gti_mir_v_1 = __gti_mir_v_2;" in constant
                        and "__gti_mir_v_1 = static_cast<std::int32_t>(42);"
                        not in constant
                    )
                else:
                    exact_transform = (
                        "__gti_mir_v_1 = static_cast<std::int32_t>(42);"
                        in constant
                        and "__gti_mir_v_1 = __gti_mir_v_2;" not in constant
                    )
                if not exact_transform:
                    sys.stderr.write(
                        f"marked {optimization}/{standard} body did not "
                        "reflect its verified MIR transform state\n"
                    )
                    return 1
                emitted_by_mode[(optimization, standard)] = generated

        for standard in ("c++20", "c++23"):
            o1_constant = function_definition(
                emitted_by_mode[("O1", standard)], "mir_constant"
            )
            o3_constant = function_definition(
                emitted_by_mode[("O3", standard)], "mir_constant"
            )
            if o1_constant != o3_constant:
                sys.stderr.write(
                    f"marked O1/O3 {standard} bodies did not preserve the "
                    "same verified MIR identity fold\n"
                )
                return 1

    chain_source = (
        "int32_t chain_leaf(int32_t value) {\n"
        "  return value + 1;\n"
        "}\n"
        "int32_t chain_caller(int32_t value) {\n"
        "  return chain_leaf(value) + 2;\n"
        "}\n"
        "int main() {\n"
        "  if (chain_caller(39) != 42) {\n"
        "    return 1;\n"
        "  }\n"
        "  [[discard]] chain_caller(2147483647);\n"
        "  return 3;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-chain-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "chain-failure.gti"
        source_path.write_text(chain_source, encoding="utf8")
        executable = root / "chain-failure"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        executed = run([str(executable)])
        # The record written inside the transformed leaf must surface
        # unchanged through the caller's Invoke edge and the boundary
        # wrapper: the clang-style report cites the leaf's line, not the
        # caller's.
        # Fragment assertions hold on both sides of the in-flight report
        # format migration: code, category, source name, and the leaf's
        # line are the contract pinned here.
        if (
            executed.returncode != 70
            or "GTI-R0001" not in executed.stderr
            or "integer_overflow" not in executed.stderr
            or "chain-failure.gti" not in executed.stderr
            or ":2" not in executed.stderr
            or executed.stdout != ""
        ):
            sys.stderr.write(
                "transformed-callee chain did not propagate the leaf's "
                f"record: exit={executed.returncode} "
                f"stderr={executed.stderr}\n"
            )
            return 1

    failing_source = (
        "int32_t checked_increment(int32_t value) {\n"
        "  return value + 1;\n"
        "}\n"
        "int main() {\n"
        "  mut int32_t total = checked_increment(41);\n"
        "  if (total != 42) {\n"
        "    return 1;\n"
        "  }\n"
        "  total = checked_increment(2147483647);\n"
        "  return 3;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-failure-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "leaf-failure.gti"
        source_path.write_text(failing_source, encoding="utf8")
        executable = root / "leaf-failure"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        executed = run([str(executable)])
        if (
            executed.returncode != 70
            or "GTI-R0001" not in executed.stderr
            or "integer_overflow" not in executed.stderr
            or executed.stdout != ""
        ):
            sys.stderr.write(
                "migrated leaf failure did not reach the defined contract: "
                f"exit={executed.returncode} stderr={executed.stderr}\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
