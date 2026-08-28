#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


FIXTURES = (
    "issue_208_literal_field_initializer.gti",
    "issue_208_vector_field_initializer.gti",
    "issue_210_mutable_subscript_reads.gti",
    "issue_211_cleanup_element_replacement.gti",
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def emit_cpp(
    compiler: pathlib.Path,
    source: pathlib.Path,
    destination: pathlib.Path,
) -> str | None:
    emitted = run(
        [
            str(compiler),
            str(source),
            "-O0",
            "--std",
            "c++23",
            "--emit-cpp",
            "-o",
            str(destination),
        ]
    )
    if emitted.returncode != 0:
        fail(f"emit C++23/O0 {source.name}", emitted)
        return None
    return destination.read_text(encoding="utf8")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issues_208_210_211_regression_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-issues-208-211-") as temporary:
        root = pathlib.Path(temporary)
        for fixture_name in FIXTURES:
            source = fixtures / fixture_name
            for optimization in ("O0", "O3"):
                for standard in ("c++20", "c++23"):
                    mode = f"{optimization}-{standard}"
                    executable = root / f"{source.stem}-{mode}"
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
                        return fail(f"build {fixture_name} {mode}", built)
                    executed = run([str(executable)])
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        return fail(f"run {fixture_name} {mode}", executed)

        literal_cpp = emit_cpp(
            compiler,
            fixtures / "issue_208_literal_field_initializer.gti",
            root / "issue-208-literal.cpp",
        )
        if literal_cpp is None:
            return 1
        literal_initializer = re.compile(
            r"holder::holder\([^\n]*\)\s*:\s*"
            r"held\(::__gti_program::leaf\(static_cast<std::int32_t>\(3\)\)\)"
        )
        if len(literal_initializer.findall(literal_cpp)) != 2:
            sys.stderr.write(
                "generated C++ lost the exact source-literal field initializer\n"
            )
            return 1
        if "(*this).held = " in literal_cpp:
            sys.stderr.write(
                "generated C++ assigns the literal-built field in the "
                "constructor body\n"
            )
            return 1

        vector_cpp = emit_cpp(
            compiler,
            fixtures / "issue_208_vector_field_initializer.gti",
            root / "issue-208-vector.cpp",
        )
        if vector_cpp is None:
            return 1
        vector_constructor = re.search(
            r"holder::holder\(\) \{(?P<body>.*?)\n  \}",
            vector_cpp,
            re.DOTALL,
        )
        if vector_constructor is None:
            sys.stderr.write("generated C++ lost holder's plain constructor\n")
            return 1
        vector_body = vector_constructor.group("body")
        # Concrete generic class families use their exact owner-qualified
        # SHA-256 identity rather than spelling the source primary template.
        slot_match = re.search(
            r"mir_lifetime_slot<"
            r"(?P<type>::__gti_program::__gti_std::"
            r"__gti_type_[0-9a-f]{64}_vector)> "
            r"(?P<slot>__gti_mir_p_\d+);",
            vector_body,
        )
        destination_match = re.search(
            r"auto &(?P<destination>__gti_mir_p_\d+) = \(\*this\)\.items;",
            vector_body,
        )
        if slot_match is None or destination_match is None:
            sys.stderr.write(
                "generated C++ lost the vector field's exact MIR slot mapping\n"
            )
            return 1
        vector_schedule = re.compile(
            re.escape(slot_match.group("slot"))
            + r"\.construct_from\(\[&\]\(\) \{ return "
            + re.escape(slot_match.group("type"))
            + r"\(\); \}\);.*?"
            + re.escape(destination_match.group("destination"))
            + r" = std::move\("
            + re.escape(slot_match.group("slot"))
            + r"\.get\(\)\);\s*"
            + re.escape(slot_match.group("slot"))
            + r"\.destroy\(\);",
            re.DOTALL,
        )
        if vector_schedule.search(vector_body) is None:
            sys.stderr.write(
                "generated C++ lost the vector field's "
                "construct/publish/drop schedule\n"
            )
            return 1

        reads_cpp = emit_cpp(
            compiler,
            fixtures / "issue_210_mutable_subscript_reads.gti",
            root / "issue-210.cpp",
        )
        if reads_cpp is None:
            return 1
        for method in ("first", "second"):
            consumed_receiver = re.compile(
                r"\(\*__gti_mir_loan_(?P<loan>\d+)\)\."
                r"::__gti_program::pair::__gti_fn_\d+_"
                + method
                + r"\(\);\s*// GTI MIR end-borrow loan (?P=loan)"
            )
            if consumed_receiver.search(reads_cpp) is None:
                sys.stderr.write(
                    f"generated C++ does not retire the {method} receiver's "
                    "transient call-result loan\n"
                )
                return 1

        replacement_cpp = emit_cpp(
            compiler,
            fixtures / "issue_211_cleanup_element_replacement.gti",
            root / "issue-211.cpp",
        )
        if replacement_cpp is None:
            return 1
        replacement = re.search(
            r"// GTI MIR activate two-phase loan (?P<loan>\d+)\s*"
            r"\(\*__gti_mir_loan_(?P=loan)\) = "
            r"std::move\((?P<slot>__gti_mir_p_\d+)\.get\(\)\);\s*"
            r"// GTI MIR end-borrow loan (?P=loan)\s*"
            r"(?:__gti_mir_drop_success_\d+ = )?"
            r"(?P=slot)\.destroy(?:_with_failure)?\([^;]*\);",
            replacement_cpp,
        )
        if replacement is None:
            sys.stderr.write(
                "generated C++ lost the cleanup-owning subscript "
                "replacement schedule\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
