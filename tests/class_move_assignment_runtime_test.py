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


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: class_move_assignment_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "class_move_assignment.gti"
    negative = fixtures / "class_move_assignment_copy_negative.gti"

    with tempfile.TemporaryDirectory(prefix="gti-class-move-assign-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(positive),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

        generated_path = root / "class-move-assignment.cpp"
        emitted = run(
            [
                str(compiler),
                str(positive),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit C++23/O0", emitted)
        generated = generated_path.read_text(encoding="utf8")
        # Replacement cleanup precedes publication. A fallible cleanup branches
        # to publication only on success; the temporary's moved-from shell is
        # retired immediately after that publication.
        schedule_pattern = re.compile(
            r"(?P<slot>__gti_mir_p_\d+)\.construct\(std::move\(__gti_mir_p_\d+\.get\(\)\)\);\n"
            r"\s*(?P<status>__gti_mir_replacement_success_\d+) = [^\n]+"
            r"\}\((?P<destination>__gti_mir_p_\d+)\.get\(\)\);\n"
            r"\s*if \((?P=status)\) \{\s*goto (?P<success>__gti_mir_bb_\d+);"
            r"\s*\} else \{\s*" + INCOMING_CLEANUP_CHECK +
            r"\s*goto (?P<failure>__gti_mir_bb_\d+);\s*\}"
        )
        schedule = list(schedule_pattern.finditer(generated))
        if len(schedule) != 3:
            sys.stderr.write(
                "generated C++ lost the staged class move-assignment "
                f"schedule (found {len(schedule)}, expected 3)\n"
            )
            return 1

        for matched in schedule:
            failure = matched.group("failure")
            if failure is None:
                continue
            # Keep the source slot's two retirement sites bound to mutually
            # exclusive replacement continuations in this one emitted body.
            start = generated.rfind("// GTI verified-MIR body:", 0, matched.start())
            end = generated.find("// GTI verified-MIR body:", matched.end())
            body = generated[start:end if end != -1 else len(generated)]
            # Source-owner failure continuations can intervene in printed
            # order. Bind publication to the exact successful branch label.
            publication_pattern = re.compile(
                re.escape(matched.group("success")) + r": \{\s*" +
                re.escape(matched.group("destination")) +
                r"\.get\(\) = std::move\(" + re.escape(matched.group("slot")) +
                r"\.get\(\)\);\s*" + re.escape(matched.group("slot")) +
                r"\.destroy\(\);"
            )
            if publication_pattern.search(body) is None:
                sys.stderr.write("move replacement lost success-only publication and immediate shell retirement\n")
                return 1
            rollback_pattern = re.compile(
                re.escape(failure) + r": \{\s*" + cleanup_scope(failure) +
                r"// GTI MIR failure-record \d+ cleanup\s*"
                r"std::memset\(&__gti_mir_cleanup_failure_record, 0, sizeof\(__gti_mir_cleanup_failure_record\)\);\s*"
                r"__gti_mir_drop_success_\d+ = " + re.escape(matched.group("slot")) +
                r"\.(?:destroy_with_failure|template destroy_unique_aggregate_with_failure<\d+>)\("
            )
            rollback = rollback_pattern.search(body)
            drops = re.findall(re.escape(matched.group("slot")) +
                               r"\.(?:destroy|destroy_with_failure|template destroy_unique_aggregate_with_failure<\d+>)\(", body)
            if rollback is None or len(drops) != 2:
                sys.stderr.write("fallible move replacement lost its exclusive source-slot rollback\n")
                return 1
            missing_guard = re.sub(INCOMING_CLEANUP_CHECK, "", matched.group(0), count=1)
            missing_scope = re.sub(cleanup_scope(failure), "", rollback.group(0), count=1)
            if (schedule_pattern.search(missing_guard) is not None or
                    rollback_pattern.search(missing_scope) is not None):
                sys.stderr.write("move replacement proof accepts missing inherited-cleanup protection\n")
                return 1

        rejected = run(
            [
                str(compiler),
                str(negative),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "move-only-copy-assign"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2003]" not in rejected.stderr
            or "Move-only owners cannot be copied" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject move-only copy assignment", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
