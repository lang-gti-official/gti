#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: global_class_storage_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(
        prefix="gti-global-class-storage-"
    ) as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, _native in native_modes():
            executable = root / mode
            built = run(
                [
                    str(compiler),
                    str(source),
                    *flags,
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                return fail(built)
            executed = run([str(executable)])
            if executed.returncode != 0:
                return fail(executed)

            emitted = root / f"{mode}.cpp"
            if is_cpp_mode(flags):
                emission = run(
                    [
                        str(compiler),
                        str(source),
                        *flags,
                        "--emit-cpp",
                        "-o",
                        str(emitted),
                    ]
                )
                if emission.returncode != 0:
                    return fail(emission)
            if is_cpp_mode(flags):
                generated = emitted.read_text(encoding="utf8")
                # This cleanup-free, statically initialized object has native
                # storage. Both functions must address that same object; no
                # runtime lifetime slot or per-function copy is necessary.
                if (
                    generated.count("::__gti_program::thing g = ::__gti_program::thing();") != 1
                    or "extern ::__gti_program::thing g;" not in generated
                    or re.search(r"__gti_mir_v_\d+ = ::__gti_program::g\.value;", generated) is None
                    or re.search(r"::__gti_program::g\.value = __gti_mir_v_\d+;", generated) is None
                    or "::__gti_program::g.get()" in generated
                ):
                    sys.stderr.write(
                        f"{mode} lost the shared global class-storage "
                        "direct passive representation\n"
                    )
                    return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
