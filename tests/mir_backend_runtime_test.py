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
                if (
                    generated.count(f"{marker} field-initializers-instance")
                    != 3
                    or generated.count(
                        f"{marker} static-field-initializers-instance"
                    )
                    != 3
                    or generated.count(f"{marker} module-instance") != 1
                    or generated.count(
                        "// GTI verified-MIR body: native-boundary-v1 "
                        "function-instance"
                    )
                    != 7
                ):
                    sys.stderr.write(
                        "generated C++ lost the verified prelude initializer, "
                        "module, or native-boundary schedules\n"
                    )
                    return 1
                checked = function_definition(
                    generated, "compatibility_checked__gti_mir_failure"
                )
                if (
                    "// GTI verified-MIR body: scalar-cfg-failure-v1" not in checked
                    or function_definition(generated, "compatibility_checked")
                ):
                    sys.stderr.write(
                        "compatibility_checked should emit only its explicit "
                        "failure-form body, without an ordinary wrapper\n"
                    )
                    return 1
                for failure_name in (
                    "checked_leaf",
                    "checked_caller",
                    "entry",
                    "release",
                    "print",
                    "println",
                ):
                    failure_body = function_definition(
                        generated, f"{failure_name}__gti_mir_failure"
                    )
                    if (
                        "// GTI verified-MIR body: scalar-cfg-failure-v1"
                        not in failure_body
                    ):
                        sys.stderr.write(
                            f"{failure_name} lost its explicit failure-form "
                            "verified-MIR body\n"
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

    member_source = (
        "class Meter {\n"
        "  int32_t raw;\n"
        "\n"
        "public:\n"
        "  Meter(int32_t input) : raw(input) {}\n"
        "  int32_t checked_scale(int32_t factor) { return this.raw * factor; }\n"
        "  int32_t through(int32_t factor) { return this.checked_scale(factor); }\n"
        "};\n"
        "int main() {\n"
        "  Meter meter = Meter(6);\n"
        "  if (meter.through(7) != 42) {\n"
        "    return 1;\n"
        "  }\n"
        "  [[discard]] meter.through(2147483647);\n"
        "  return 3;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-member-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "member-chain.gti"
        source_path.write_text(member_source, encoding="utf8")
        executable = root / "member-chain"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        emitted = root / "member-chain.cpp"
        emission = run(
            [
                str(compiler),
                str(source_path),
                "--emit-cpp",
                "-o",
                str(emitted),
            ]
        )
        if emission.returncode != 0:
            return fail(emission)
        generated = emitted.read_text(encoding="utf8")
        # The receiver-carrying call spells its staged borrowed place and
        # the qualified transformed member name; both member bodies emit
        # in the failure form.
        if (
            "stages a borrowed place" not in generated
            or "(*this).::__gti_program::Meter::" not in generated
            or "checked_scale__gti_mir_failure(" not in generated
            or generated.count(
                "// GTI verified-MIR body: scalar-cfg-failure-v1"
            )
            < 2
        ):
            sys.stderr.write(
                "the member chain should reach checked_scale through the "
                "transformed receiver-call convention: a staged borrowed "
                "receiver and the qualified derived member name\n"
            )
            return 1
        executed = run([str(executable)])
        # The record written inside the transformed member must surface
        # through the caller's receiver call and the boundary wrapper: the
        # report cites checked_scale's multiplication, not the caller's
        # line, and the success path returned 42 before the failing call.
        if (
            executed.returncode != 70
            or "GTI-R0001" not in executed.stderr
            or "integer_overflow" not in executed.stderr
            or "member-chain.gti" not in executed.stderr
            or ":6" not in executed.stderr
            or executed.stdout != ""
        ):
            sys.stderr.write(
                "transformed receiver-call chain did not propagate the "
                f"member's record: exit={executed.returncode} "
                f"stderr={executed.stderr}\n"
            )
            return 1

    generic_member_source = (
        "#include <std/vector>\n"
        "int main() {\n"
        "  mut std::vector<int32_t> values = std::vector<int32_t>();\n"
        "  values.push_back(7);\n"
        "  values.erase(std::size_t(0));\n"
        "  if (!values.empty()) {\n"
        "    return 1;\n"
        "  }\n"
        "  values.push_back(9);\n"
        "  values.erase(std::size_t(4));\n"
        "  return 3;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-generic-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "generic-member.gti"
        source_path.write_text(generic_member_source, encoding="utf8")
        executable = root / "generic-member"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        emitted = run(
            [str(compiler), str(source_path), "--emit-cpp", "-o",
             str(root / "generic-member.cpp")]
        )
        generated = (root / "generic-member.cpp").read_text(encoding="utf8")
        if (
            emitted.returncode != 0
            or "mir_failure_constructor_tag_v1<" not in generated
            or "scalar-cfg-constructor-failure-v1 constructor-instance"
            not in generated
            or "push_back__gti_mir_failure(" not in generated
            or "erase__gti_mir_failure(" not in generated
            or "// GTI MIR reparent into p" not in generated
        ):
            sys.stderr.write(
                "the generic owner should publish its transformed "
                "constructor and closed member-call component\n"
            )
            return 1
        executed = run([str(executable)])
        # The generic vector owner's constructor and erase emit as explicit
        # transformed specializations, so its storage detector reports the
        # defined contract. The record cites the stdlib site, not a legacy
        # storage abort, and successful construction engages the owner once.
        if (
            executed.returncode != 70
            or "GTI-R0007" not in executed.stderr
            or "index_out_of_bounds" not in executed.stderr
            or "private_storage" not in executed.stderr
            or "vector" not in executed.stderr
            or executed.stdout != ""
        ):
            sys.stderr.write(
                "generic-owner transformed member did not report the "
                f"defined storage contract: exit={executed.returncode} "
                f"stderr={executed.stderr}\n"
            )
            return 1

    chainprint_source = (
        "int main() {\n"
        "  std::println(42);\n"
        "  return 0;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-chainprint-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "chain-print.gti"
        source_path.write_text(chainprint_source, encoding="utf8")
        executable = root / "chain-print"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        emitted = run(
            [str(compiler), str(source_path), "--emit-cpp", "-o",
             str(root / "chain-print.cpp")]
        )
        generated = (root / "chain-print.cpp").read_text(encoding="utf8")
        # The integer print chain publishes concrete transformed siblings
        # backed by verified MIR, with the array-walking digit printer in
        # the transformed failure form.
        if (
            emitted.returncode != 0
            or generated.count(
                "scalar-cfg-failure-v1 function-instance") < 3
            or "print_integral__gti_mir_failure(" not in generated
            or "mir_checked_array_read_v1(" not in generated
        ):
            sys.stderr.write(
                "integer println should emit per-instance specializations "
                "from verified MIR\n"
            )
            return 1
        executed = run([str(executable)])
        if executed.returncode != 0 or executed.stdout != "42\n":
            sys.stderr.write(
                "the specialized print chain changed observable behavior: "
                f"exit={executed.returncode} stdout={executed.stdout!r}\n"
            )
            return 1

    bounds_source = (
        "uint8_t pick(uint64_t index) {\n"
        "  mut uint8_t[4] values = {};\n"
        "  values[0] = 7;\n"
        "  return values[index];\n"
        "}\n"
        "int main() {\n"
        "  if (pick(uint64_t(0)) != 7) {\n"
        "    return 1;\n"
        "  }\n"
        "  [[discard]] pick(uint64_t(9));\n"
        "  return 3;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-bounds-") as temporary:
        root = pathlib.Path(temporary)
        source_path = root / "bounds-failure.gti"
        source_path.write_text(bounds_source, encoding="utf8")
        executable = root / "bounds-failure"
        built = run([str(compiler), str(source_path), "-o", str(executable)])
        if built.returncode != 0:
            return fail(built)
        generated = run(
            [str(compiler), str(source_path), "--emit-cpp", "-o",
             str(root / "bounds.cpp")]
        )
        if (
            generated.returncode != 0
            or "mir_checked_array_read_v1("
            not in (root / "bounds.cpp").read_text(encoding="utf8")
        ):
            sys.stderr.write(
                "the bounds fixture should read its element through the "
                "checked fixed-array helper from verified MIR\n"
            )
            return 1
        executed = run([str(executable)])
        if (
            executed.returncode != 70
            or "GTI-R0007" not in executed.stderr
            or "index_out_of_bounds" not in executed.stderr
            or ":4" not in executed.stderr
        ):
            sys.stderr.write(
                "the dynamic out-of-bounds read did not reach the defined "
                f"contract: exit={executed.returncode} "
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
