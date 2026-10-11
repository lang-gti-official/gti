#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=scaled(180), check=False
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issue_123_cleanup_temporary_argument_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "issue_123_cleanup_temporary_argument.gti"
    failure = fixtures / "issue_123_cleanup_temporary_argument_failure.gti"
    named_copy = fixtures / "issue_123_move_only_named_copy.gti"
    thread = fixtures / "issue_123_thread_handoff.gti"

    with tempfile.TemporaryDirectory(prefix="gti-issue-123-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O2"):
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

        generated_path = root / "issue-123.cpp"
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
        extraction = re.search(
            r" = ::gti_internal::backend::mir_expected_value_move_v1\("
            r"[^,]+, &(__gti_mir_p_\d+)\);",
            generated,
        )
        if extraction is None:
            sys.stderr.write(
                "generated C++ lost the owning Expected extraction slot\n"
            )
            return 1
        slot = extraction.group(1)
        if (
            len(
                re.findall(
                    r" = ::gti_internal::backend::mir_expected_value_move_v1\(",
                    generated,
                )
            )
            != 1
            or generated.count(f"std::move({slot}.get())") != 1
            or generated.count(f"{slot}.destroy();") != 1
        ):
            sys.stderr.write(
                "generated C++ duplicated owning extraction construction, "
                "consumption, or cleanup\n"
            )
            return 1

        failed_executable = root / "failure"
        built_failure = run(
            [
                str(compiler),
                str(failure),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(failed_executable),
            ]
        )
        if built_failure.returncode != 0:
            return fail("build failure cleanup fixture", built_failure)
        executed_failure = run([str(failed_executable)])
        if (
            executed_failure.returncode != 70
            or executed_failure.stdout != "41\n"
            or executed_failure.stderr.count("error[GTI-R0009]") != 1
            or "invalid_expected_access" not in executed_failure.stderr
            or "value_on_error" not in executed_failure.stderr
        ):
            return fail("run failure cleanup fixture", executed_failure)

        rejected = run(
            [
                str(compiler),
                str(named_copy),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "named-copy"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2018]" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject named move-only copy", rejected)

        thread_executable = root / "thread"
        built_thread = run(
            [
                str(compiler),
                "--execution-profile",
                "concurrent",
                "--std",
                "c++23",
                "-O2",
                str(thread),
                "-o",
                str(thread_executable),
            ]
        )
        if built_thread.returncode != 0:
            return fail("build thread handoff", built_thread)
        executed_thread = run([str(thread_executable)])
        if (
            executed_thread.returncode != 0
            or executed_thread.stdout != ""
            or executed_thread.stderr != ""
        ):
            return fail("run thread handoff", executed_thread)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
