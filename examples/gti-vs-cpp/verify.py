#!/usr/bin/env python3
"""Compile and run the paired GTI/C++ evidence examples."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Sequence


SHOWCASE_ROOT = Path(__file__).resolve().parent
REPOSITORY_ROOT = SHOWCASE_ROOT.parents[1]
MANIFEST_PATH = SHOWCASE_ROOT / "cases.json"


class VerificationFailure(RuntimeError):
    pass


def load_cases() -> list[dict[str, Any]]:
    with MANIFEST_PATH.open("r", encoding="utf-8") as manifest_file:
        manifest = json.load(manifest_file)
    if manifest.get("schema") != 1 or not isinstance(manifest.get("cases"), list):
        raise VerificationFailure(f"unsupported manifest: {MANIFEST_PATH}")
    return manifest["cases"]


def resolve_tool(requested: str | None, fallback: Path | str, label: str) -> str:
    candidate = requested or str(fallback)
    if "/" in candidate or "\\" in candidate:
        path = Path(candidate).expanduser().resolve()
        if path.is_file():
            return str(path)
    else:
        resolved = shutil.which(candidate)
        if resolved is not None:
            return resolved
    raise VerificationFailure(f"could not find {label}: {candidate}")


def run_command(
    command: Sequence[str], timeout: int = 60
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command),
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise VerificationFailure(
            f"command timed out after {timeout} seconds: {' '.join(command)}"
        ) from error


def combined_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def require_contains(output: str, fragment: str, context: str) -> None:
    if fragment.casefold() not in output.casefold():
        raise VerificationFailure(
            f"{context} did not contain {fragment!r}\n--- output ---\n{output}"
        )


def verify_expectation(
    compile_command: list[str],
    executable: Path,
    expectation: dict[str, Any],
    context: str,
) -> None:
    compile_result = run_command(compile_command)
    kind = expectation.get("kind")

    if kind == "compile_failure":
        if compile_result.returncode == 0:
            raise VerificationFailure(f"{context} unexpectedly compiled")
        require_contains(
            combined_output(compile_result), expectation["contains"], context
        )
        return

    if compile_result.returncode != 0:
        raise VerificationFailure(
            f"{context} failed to compile\n"
            f"command: {' '.join(compile_command)}\n"
            f"--- output ---\n{combined_output(compile_result)}"
        )

    if kind == "compile_success":
        return

    if kind not in {"run_success", "runtime_failure"}:
        raise VerificationFailure(f"{context} has unknown expectation {kind!r}")

    run_result = run_command([str(executable)])
    if kind == "run_success":
        expected_exit = int(expectation.get("exit_code", 0))
        if run_result.returncode != expected_exit:
            raise VerificationFailure(
                f"{context} returned {run_result.returncode}, "
                f"expected {expected_exit}\n"
                f"--- output ---\n{combined_output(run_result)}"
            )
        return

    if run_result.returncode == 0:
        raise VerificationFailure(f"{context} unexpectedly completed successfully")
    require_contains(combined_output(run_result), expectation["contains"], context)


def verify_case(
    case: dict[str, Any], gti: str, cxx: str, output_root: Path
) -> None:
    case_id = case["id"]
    for language in ("gti", "cpp"):
        specification = case[language]
        source = SHOWCASE_ROOT / specification["source"]
        if not source.is_file():
            raise VerificationFailure(f"missing source for {case_id}: {source}")
        executable = output_root / f"{case_id}-{language}"
        if language == "gti":
            command = [gti, str(source), "-O0", "-o", str(executable)]
        else:
            command = [
                cxx,
                "-std=c++20",
                "-O0",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                str(source),
                "-o",
                str(executable),
            ]
        verify_expectation(
            command,
            executable,
            specification["expect"],
            f"{case_id} ({language})",
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the paired GTI versus C++ evidence examples."
    )
    parser.add_argument("--gti", help="path to the GTI compiler")
    parser.add_argument("--cxx", help="path or command name for the C++ compiler")
    parser.add_argument(
        "--case",
        action="append",
        dest="case_ids",
        help="verify only this case (repeatable)",
    )
    parser.add_argument("--list", action="store_true", help="list cases and exit")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        cases = load_cases()
        if arguments.list:
            for case in cases:
                print(f"{case['id']}: {case['claim']}")
            return 0

        if arguments.case_ids:
            selected = set(arguments.case_ids)
            known = {case["id"] for case in cases}
            unknown = selected - known
            if unknown:
                raise VerificationFailure(
                    "unknown case(s): " + ", ".join(sorted(unknown))
                )
            cases = [case for case in cases if case["id"] in selected]

        gti = resolve_tool(arguments.gti, REPOSITORY_ROOT / "build" / "gti", "GTI")
        cxx = resolve_tool(arguments.cxx, "c++", "C++ compiler")

        with tempfile.TemporaryDirectory(prefix="gti-vs-cpp-") as temporary:
            output_root = Path(temporary)
            for case in cases:
                verify_case(case, gti, cxx, output_root)
                print(f"PASS {case['id']}: {case['claim']}")
        print(f"Verified {len(cases)} paired comparison case(s).")
        return 0
    except VerificationFailure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
