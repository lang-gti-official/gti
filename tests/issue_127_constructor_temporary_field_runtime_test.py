#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


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
            "usage: issue_127_constructor_temporary_field_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = (
        pathlib.Path(sys.argv[2]).resolve()
        / "issue_127_constructor_temporary_field.gti"
    )

    with tempfile.TemporaryDirectory(prefix="gti-issue-127-") as temporary:
        root = pathlib.Path(temporary)
        modes = [(level, []) for level in ("O0", "O1", "O2", "O3")]
        modes.append(("O1", ["-fsanitize=address,undefined"]))
        for optimization, native_flags in modes:
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                if native_flags:
                    mode += "-sanitized"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(fixture),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                        "--",
                        "-fno-elide-constructors",
                        *native_flags,
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

        generated_path = root / "issue-127.cpp"
        emitted = run(
            [
                str(compiler),
                str(fixture),
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
        direct_field_reads = re.findall(
            r"__gti_mir_v_\d+\.get\(\)\.value", generated
        )
        direct_slot_constructions = re.findall(
            r"::new \(static_cast<void \*>\(__gti_mir_v_\d+\.construction_address\(\)\)\) "
            r"[\w:]+\("
            r"::gti_internal::backend::mir_failure_constructor_tag_v1<",
            generated,
        )
        if len(direct_field_reads) != 11 or len(direct_slot_constructions) != 9:
            sys.stderr.write(
                "generated C++ lost the exact constructor/call value-rooted "
                "field-read slots\n"
            )
            return 1
        # A temporary whose class has a destructor is read through MIR's own
        # result slot, never through a cleanup-free value slot.
        owned_field_reads = re.findall(
            r"__gti_mir_p_\d+\.get\(\)\.(?:count|tag)\b", generated
        )
        cleanup_free_owned_reads = re.findall(
            r"__gti_mir_v_\d+\.get\(\)\.(?:count|tag)\b", generated
        )
        if len(owned_field_reads) != 18 or cleanup_free_owned_reads:
            sys.stderr.write(
                "generated C++ lost the owned result slots of destructor-"
                "bearing constructor temporaries\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
