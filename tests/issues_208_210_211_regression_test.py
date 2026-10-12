#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


INCOMING_CLEANUP_CHECK = (
    r"::gti_internal::backend::mir_check_incoming_cleanup_failure_v1\("
    r"__gti_mir_failure_record, "
    r"&::gti_internal::backend::__gti_failure_artifact_descriptor_v1\);"
)


def cleanup_scope(_block: str) -> str:
    return (
        r"__gti_mir_cleanup_context\.activate\(__gti_mir_failure_record, "
        r"&::gti_internal::backend::__gti_failure_artifact_descriptor_v1\);\s*"
    )


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
        # A constructor may need only its checked entry point. Require the
        # source literal in every emitted definition, without requiring an
        # unused ordinary ABI definition solely for a fixed text count.
        constructor_definitions = re.findall(
            r"^\s*holder::holder\(", literal_cpp, re.MULTILINE
        )
        if (not constructor_definitions or
                len(literal_initializer.findall(literal_cpp)) !=
                len(constructor_definitions)):
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
            r"holder::holder\(::gti_internal::backend::mir_failure_constructor_tag_v1<\d+>,[^\n]*\) : items\([^\n]*\) \{(?P<body>.*?)\n  \}",
            vector_cpp,
            re.DOTALL,
        )
        if vector_constructor is None:
            sys.stderr.write("generated C++ lost holder's failure-form constructor\n")
            return 1
        vector_body = vector_constructor.group("body")
        destination_match = re.search(
            r"auto &(?P<destination>__gti_mir_p_\d+) = \(\*this\)\.items;",
            vector_body,
        )
        if destination_match is None:
            sys.stderr.write(
                "generated C++ lost the vector field's exact MIR slot mapping\n"
            )
            return 1
        # The value and rollback obligations now share final field storage:
        # destroy only the empty representation, construct in place, and
        # publish on the successful Invoke edge. No moved temporary survives.
        destination = re.escape(destination_match.group("destination"))
        vector_schedule_pattern = re.compile(
            r"std::destroy_at\(std::addressof\(" + destination + r"\)\);\s*"
            r"::new \(static_cast<void \*>\(std::addressof\(" + destination + r"\)\)\) "
            r"::__gti_program::__gti_std::"
            r"(?:__gti_type_[0-9a-f]{64}_vector|vector<std::int32_t>)\("
            r"::gti_internal::backend::mir_failure_constructor_tag_v1<\d+>\{\}, "
            r"&(?P<status>__gti_mir_construct_success_\d+), __gti_mir_failure_record\);\s*"
            r"if \((?P=status)\) \{\s*goto (?P<success>__gti_mir_bb_\d+);\s*"
            r"\} else \[\[unlikely\]\] \{\s*" + INCOMING_CLEANUP_CHECK +
            r"\s*goto (?P<failure>__gti_mir_bb_\d+);"
        )
        vector_schedule = vector_schedule_pattern.search(vector_body)
        if vector_schedule is None:
            sys.stderr.write(
                "generated C++ lost the vector field's "
                "in-place construction and checked publication schedule\n"
            )
            return 1
        field_id = destination_match.group("destination").removeprefix("__gti_mir_p_")
        success = re.search(
            re.escape(vector_schedule.group("success")) + r": \{\s*"
            r"// GTI MIR in-place constructor field reparent into p" + field_id + r"\s*"
            r"// GTI MIR full-expression boundary \d+\s*"
            r"// GTI MIR cleanup boundary \d+\s*"
            r"\*__gti_mir_constructor_success = true;\s*return;", vector_body
        )
        failure_pattern = re.compile(
            re.escape(vector_schedule.group("failure")) + r": \{\s*"
            + r"// GTI MIR failure-record \d+ cleanup\s*"
            r"// GTI MIR propagate failure-record \d+ after cleanup\s*return;"
        )
        failure = failure_pattern.search(vector_body)
        # This constructor has no local cleanup on its failure edge. An
        # inherited failure still needs the incoming guard above, but a local
        # cleanup context would add cost without protecting any operation.
        if (success is None or failure is None or " = std::move(" in vector_body or
                "__gti_mir_cleanup_context" in vector_body):
            sys.stderr.write("vector field must publish only on success, without a second move\n")
            return 1
        missing_guard = re.sub(INCOMING_CLEANUP_CHECK, "", vector_schedule.group(0), count=1)
        missing_propagation = re.sub(r"// GTI MIR propagate failure-record \d+ after cleanup", "", failure.group(0), count=1)
        if (vector_schedule_pattern.search(missing_guard) is not None or
                failure_pattern.search(missing_propagation) is not None):
            sys.stderr.write("field publication proof accepts missing inherited-cleanup protection\n")
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
        cleanup = re.search(
            r"// GTI MIR activate two-phase loan (?P<loan>\d+)\s*"
            r"(?P<status>__gti_mir_replacement_success_\d+) = [^\n]+"
            r"\}\(\(\*__gti_mir_loan_(?P=loan)\)\);\s*"
            r"if \((?P=status)\) \{\s*goto (?P<success>__gti_mir_bb_\d+);"
            r"\s*\} else \[\[unlikely\]\] \{\s*" + INCOMING_CLEANUP_CHECK +
            r"\s*goto (?P<failure>__gti_mir_bb_\d+);",
            replacement_cpp,
        )
        if cleanup is None:
            sys.stderr.write("missing checked subscript replacement cleanup\n")
            return 1
        replacement = re.search(
            re.escape(cleanup.group("success")) + r": \{\s*"
            r"\(\*__gti_mir_loan_(?P<loan>" + cleanup.group("loan") + r")\) = "
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
        rollback = re.search(
            re.escape(cleanup.group("failure")) + r": \{\s*"
            + cleanup_scope(cleanup.group("failure")) +
            r"// GTI MIR failure-record \d+ cleanup\s*"
            r"// GTI MIR end-borrow loan " + cleanup.group("loan") + r"\s*"
            r"std::memset\(&__gti_mir_cleanup_failure_record, 0, sizeof\(__gti_mir_cleanup_failure_record\)\);\s*"
            r"__gti_mir_drop_success_\d+ = "
            + re.escape(replacement.group("slot"))
            + r"\.destroy_with_failure\(",
            replacement_cpp,
        )
        if rollback is None:
            sys.stderr.write("missing unpublished subscript value rollback\n")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
