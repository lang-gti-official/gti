#!/usr/bin/env python3

import os
import pathlib
import re
import subprocess
import sys
import tempfile


MARKER = "// GTI verified-MIR body: scalar-cfg-failure-v1"


def report_pattern(
    source: pathlib.Path,
    code: str,
    category: str,
    line: int,
    start: int,
    end: int,
    detail: str,
) -> re.Pattern[bytes]:
    # The report no longer carries the artifact identity; stability of that
    # identity is checked against the emitted C++ descriptor instead.
    del start, end
    return re.compile(
        rb"^"
        + re.escape(source.name.encode())
        + rb":"
        + str(line).encode()
        + rb": runtime error\["
        + code.encode()
        + rb"\]: "
        + category.encode()
        + rb" in "
        + detail.encode()
        + rb"\n$"
    )


def run(command: list[str]) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[bytes]) -> int:
    sys.stderr.buffer.write(process.stdout)
    sys.stderr.buffer.write(process.stderr)
    return 1


def validate_emission(
    generated: str,
    expected_bodies: int,
    label: str,
    optimization: str,
    standard: str,
) -> bool:
    if generated.count(MARKER) != expected_bodies:
        sys.stderr.write(
            f"{label} {optimization}/{standard} did not select the exact "
            f"{expected_bodies}-body hosted failure component\n"
        )
        return False
    required = (
        "std::int32_t *__gti_mir_out_result",
        "::gti_failure_record_v1 *__gti_mir_failure_record",
        "GTI_FAILURE_EXIT_STATUS",
        "catch (...)",
    )
    for spelling in required:
        if spelling not in generated:
            sys.stderr.write(
                f"{optimization}/{standard} omitted hosted failure spelling "
                f"{spelling!r}\n"
            )
            return False
    signature = re.compile(
        r"  bool (?:__gti_entry|__gti_fn_[0-9]+_[A-Za-z_][A-Za-z0-9_]*)"
        r"__gti_mir_failure\("
        r"[^{};]*std::int32_t \*__gti_mir_out_result, "
        r"::gti_failure_record_v1 \*__gti_mir_failure_record\) \{\n"
        r"    "
        + re.escape(MARKER)
    )
    if len(signature.findall(generated)) != expected_bodies:
        sys.stderr.write(
            f"{label} {optimization}/{standard} did not emit the exact "
            "bool/out-result/record hidden ABI for every selected body\n"
        )
        return False
    # ADR 017: each per-body defined-failure boundary wrapper carries its
    # own termination call, so the whole-artifact count is no longer one;
    # the hosted main must still own exactly one.
    main_index = generated.find("int main()")
    if (
        main_index == -1
        or generated[main_index:].count("::gti_rt_failure_terminate_v1(") != 1
    ):
        sys.stderr.write(
            f"{label} {optimization}/{standard} hosted main did not own "
            "exactly one termination call\n"
        )
        return False
    firewall = (
        "catch (...) {\n"
        "    std::_Exit(GTI_FAILURE_EXIT_STATUS);\n"
        "  }"
    )
    if firewall not in generated:
        sys.stderr.write(
            f"{label} {optimization}/{standard} did not emit the immediate "
            "native-exception firewall\n"
        )
        return False
    catch_at = generated.index("catch (...)")
    catch_body = generated[catch_at : generated.index("}", catch_at) + 1]
    if "__gti_failure_record" in catch_body or "gti_rt_failure" in catch_body:
        sys.stderr.write(
            f"{label} {optimization}/{standard} forged or reported a GTI "
            "record from the native-exception firewall\n"
        )
        return False
    return True


def compile_program(
    compiler: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    optimization: str,
    standard: str,
) -> subprocess.CompletedProcess[bytes]:
    return run(
        [
            str(compiler),
            str(source),
            f"-{optimization}",
            "--std",
            standard,
            "-o",
            str(output),
        ]
    )


def artifact_identity(emitted: str) -> str:
    """The artifact identity byte list from the emitted failure descriptor.

    The runtime report no longer prints this identity, so stability across
    optimization levels and C++ standards is checked at its source instead.
    """
    match = re.search(
        r"__gti_failure_artifact_descriptor_v1 = \{.*?\{([0-9, ]+)\}",
        emitted,
        re.DOTALL,
    )
    assert match is not None, "emitted C++ has no failure artifact descriptor"
    return match.group(1)


def emit_program(
    compiler: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    optimization: str,
    standard: str,
) -> subprocess.CompletedProcess[bytes]:
    return run(
        [
            str(compiler),
            str(source),
            f"-{optimization}",
            "--std",
            standard,
            "--emit-cpp",
            "-o",
            str(output),
        ]
    )


def main() -> int:
    if len(sys.argv) != 21:
        raise SystemExit(
            "usage: mir_backend_scalar_failure_callgraph_runtime_test.py "
            "<gti> <lifecycle-success-source> <all-operations-success-source> "
            "<addition-overflow-source> "
            "<division-zero-source> <division-overflow-source> "
            "<negative-left-shift-source> <wide-left-shift-source> "
            "<subtraction-overflow-source> <multiplication-overflow-source> "
            "<remainder-zero-source> <negative-right-shift-source> "
            "<wide-right-shift-source> <negation-overflow-source> "
            "<wide-conversion-source> <negative-conversion-source> "
            "<unsigned-addition-source> <unsigned-subtraction-source> "
            "<unsigned-multiplication-source> <owning-array-parameter-source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    success_cases = (
        ("lifecycle-success", pathlib.Path(sys.argv[2]).resolve(), 3),
        ("all-operations-success", pathlib.Path(sys.argv[3]).resolve(), 2),
        ("owning-array-parameter", pathlib.Path(sys.argv[20]).resolve(), 3),
    )
    failure_cases = (
        (
            "addition-overflow",
            pathlib.Path(sys.argv[4]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            20,
            432,
            433,
            "addition",
            3,
        ),
        (
            "division-zero",
            pathlib.Path(sys.argv[5]).resolve(),
            "GTI-R0002",
            "division_by_zero",
            2,
            72,
            73,
            "integer_division",
            2,
        ),
        (
            "division-overflow",
            pathlib.Path(sys.argv[6]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            2,
            72,
            73,
            "division",
            2,
        ),
        (
            "negative-shift",
            pathlib.Path(sys.argv[7]).resolve(),
            "GTI-R0004",
            "negative_shift_count",
            2,
            69,
            71,
            "left_shift",
            2,
        ),
        (
            "wide-shift",
            pathlib.Path(sys.argv[8]).resolve(),
            "GTI-R0005",
            "shift_count_out_of_range",
            2,
            69,
            71,
            "left_shift",
            2,
        ),
        (
            "subtraction-overflow",
            pathlib.Path(sys.argv[9]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            2,
            73,
            74,
            "subtraction",
            2,
        ),
        (
            "multiplication-overflow",
            pathlib.Path(sys.argv[10]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            2,
            73,
            74,
            "multiplication",
            2,
        ),
        (
            "remainder-zero",
            pathlib.Path(sys.argv[11]).resolve(),
            "GTI-R0003",
            "modulo_by_zero",
            2,
            75,
            76,
            "integer_modulo",
            2,
        ),
        (
            "negative-right-shift",
            pathlib.Path(sys.argv[12]).resolve(),
            "GTI-R0004",
            "negative_shift_count",
            2,
            69,
            71,
            "right_shift",
            2,
        ),
        (
            "wide-right-shift",
            pathlib.Path(sys.argv[13]).resolve(),
            "GTI-R0005",
            "shift_count_out_of_range",
            2,
            69,
            71,
            "right_shift",
            2,
        ),
        (
            "negation-overflow",
            pathlib.Path(sys.argv[14]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            2,
            49,
            50,
            "negation",
            2,
        ),
        (
            "wide-conversion",
            pathlib.Path(sys.argv[15]).resolve(),
            "GTI-R0006",
            "numeric_conversion_out_of_range",
            5,
            105,
            112,
            "numeric_cast",
            1,
        ),
        (
            "negative-conversion",
            pathlib.Path(sys.argv[16]).resolve(),
            "GTI-R0006",
            "numeric_conversion_out_of_range",
            3,
            61,
            69,
            "numeric_cast",
            1,
        ),
        (
            "unsigned-addition-overflow",
            pathlib.Path(sys.argv[17]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            4,
            94,
            95,
            "addition",
            1,
        ),
        (
            "unsigned-subtraction-overflow",
            pathlib.Path(sys.argv[18]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            4,
            79,
            80,
            "subtraction",
            1,
        ),
        (
            "unsigned-multiplication-overflow",
            pathlib.Path(sys.argv[19]).resolve(),
            "GTI-R0001",
            "integer_overflow",
            4,
            94,
            95,
            "multiplication",
            1,
        ),
    )

    with tempfile.TemporaryDirectory(prefix="gti-mir-scalar-failure-") as temporary:
        root = pathlib.Path(temporary)
        standard_library = root / "stdlib"
        standard_library.mkdir()
        (standard_library / "prelude.gti").write_text("", encoding="utf8")
        os.environ["GTI_STDLIB_PATH"] = str(standard_library)
        observed_identities: dict[str, bytes] = {}
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                for label, source, _ in success_cases:
                    success_executable = root / (
                        f"{label}-{optimization}-{standard}"
                    )
                    built = compile_program(
                        compiler,
                        source,
                        success_executable,
                        optimization,
                        standard,
                    )
                    if built.returncode != 0:
                        return fail(built)
                    succeeded = run([str(success_executable)])
                    if (
                        succeeded.returncode != 0
                        or succeeded.stdout != b""
                        or succeeded.stderr != b""
                    ):
                        return fail(succeeded)

                for (
                    label,
                    source,
                    code,
                    category,
                    line,
                    start,
                    end,
                    detail,
                    expected_bodies,
                ) in failure_cases:
                    failure_executable = root / (
                        f"{label}-{optimization}-{standard}"
                    )
                    built = compile_program(
                        compiler,
                        source,
                        failure_executable,
                        optimization,
                        standard,
                    )
                    if built.returncode != 0:
                        return fail(built)
                    failed = run([str(failure_executable)])
                    match = report_pattern(
                        source, code, category, line, start, end, detail
                    ).fullmatch(failed.stderr)
                    if (
                        failed.returncode != 70
                        or failed.stdout != b""
                        or match is None
                    ):
                        return fail(failed)
                    identity_cpp = (
                        root / f"{label}-{optimization}-{standard}-id.cpp"
                    )
                    identity_emission = emit_program(
                        compiler, source, identity_cpp, optimization, standard
                    )
                    if identity_emission.returncode != 0:
                        return fail(identity_emission)
                    identity = artifact_identity(
                        identity_cpp.read_text(encoding="utf8")
                    )
                    if label not in observed_identities:
                        observed_identities[label] = identity
                    elif identity != observed_identities[label]:
                        sys.stderr.write(
                            f"{label} artifact identity changed across "
                            "optimization or C++ standard\n"
                        )
                        return 1

                for label, source, expected_bodies in (
                    *success_cases,
                    *((case[0], case[1], case[8]) for case in failure_cases),
                ):
                    emitted = root / f"{label}-{optimization}-{standard}.cpp"
                    emission = emit_program(
                        compiler, source, emitted, optimization, standard
                    )
                    if emission.returncode != 0:
                        return fail(emission)
                    if not validate_emission(
                        emitted.read_text(encoding="utf8"),
                        expected_bodies,
                        label,
                        optimization,
                        standard,
                    ):
                        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
