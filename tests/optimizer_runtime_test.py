#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(arguments: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        arguments,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: optimizer_runtime_test.py "
            "<gti> <source> <cxx> <source-root> <runtime-library>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    native_compiler = pathlib.Path(sys.argv[3]).absolute()
    source_root = pathlib.Path(sys.argv[4]).resolve()
    runtime_library = pathlib.Path(sys.argv[5]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-optimizer-runtime-") as directory:
        root = pathlib.Path(directory)
        mir_by_level: dict[str, str] = {}
        cpp_by_level: dict[str, str] = {}
        for level in ("O0", "O1", "O2", "O3"):
            mir = root / f"optimizer-{level}.mir"
            run(
                [
                    str(compiler),
                    str(source),
                    f"-{level}",
                    "--emit-mir",
                    "-o",
                    str(mir),
                ]
            )
            mir_by_level[level] = mir.read_text(encoding="utf-8")

            emitted_cpp = root / f"optimizer-{level}.cpp"
            run(
                [
                    str(compiler),
                    str(source),
                    f"-{level}",
                    "--emit-cpp",
                    "-o",
                    str(emitted_cpp),
                ]
            )
            cpp_by_level[level] = emitted_cpp.read_text(encoding="utf-8")
            executable = root / f"optimizer-{level}"
            native_command = [
                str(native_compiler),
                "-std=c++23",
                "-O0",
                "-fno-fast-math",
                "-ffp-contract=off",
                "-D__gti_strict_ieee754=1",
                f"-I{source_root / 'runtime' / 'include'}",
                f"-I{source_root / 'vendor' / 'expected_lite' / 'include'}",
                str(emitted_cpp),
                str(runtime_library),
                "-o",
                str(executable),
            ]
            if sys.platform != "win32":
                native_command.append("-pthread")
            run(
                native_command
            )
            run([str(executable)])

        assert mir_by_level["O0"] != mir_by_level["O1"]
        assert mir_by_level["O1"] != mir_by_level["O2"]
        assert mir_by_level["O2"] == mir_by_level["O3"]
        assert "safety-discharges=[arithmetic-overflow:" not in mir_by_level["O0"]
        assert "safety-discharges=[arithmetic-overflow:" not in mir_by_level["O1"]
        assert "safety-discharges=[arithmetic-overflow:" in mir_by_level["O2"]
        assert "safety-discharges=[arithmetic-overflow:" in mir_by_level["O3"]
        assert "parameters=[{%" not in mir_by_level["O0"]
        assert "parameters=[{%" in mir_by_level["O1"]
        assert "target-arguments=[operand(" in mir_by_level["O1"]
        assert "tombstone=1" in mir_by_level["O1"]
        assert "__gti_mir_edge_" in cpp_by_level["O1"]
        assert "__gti_mir_v_" in cpp_by_level["O1"]

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
