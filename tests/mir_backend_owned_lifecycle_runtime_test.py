#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, native_modes, optimization_level


MARKER = "// GTI verified-MIR body: scalar-cfg"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def body_containing(generated: str, spelling: str) -> str:
    position = generated.find(spelling)
    while position != -1:
        line_end = generated.find("\n", position)
        brace = generated.find(" {\n", position)
        if brace != -1 and (line_end == -1 or brace < line_end):
            end = generated.find("\n  }", brace)
            return "" if end == -1 else generated[brace : end + 4]
        position = generated.find(spelling, position + len(spelling))
    return ""


def validate_family(generated: str, optimization: str, mode: str) -> bool:
    selected = ("_consume", "_run_scopes", "ScopeFlag::ScopeFlag")
    for spelling in selected:
        if MARKER not in body_containing(generated, spelling):
            sys.stderr.write(
                f"{optimization}/{mode} did not emit {spelling} from "
                "general MIR\n"
            )
            return False
    if MARKER not in body_containing(
        generated, "void ScopeFlag::__gti_lifecycle_cleanup_"
    ):
        sys.stderr.write(
            f"{optimization}/{mode} did not emit ScopeFlag cleanup from "
            "general MIR\n"
        )
        return False
    remaining = ("_compatibility_default_field", "_compatibility_checked")
    for spelling in remaining:
        if MARKER not in body_containing(generated, spelling):
            sys.stderr.write(
                f"{optimization}/{mode} left {spelling} outside general "
                "MIR emission\n"
            )
            return False
    if "std::move" not in body_containing(generated, "_run_scopes("):
        sys.stderr.write(
            f"{optimization}/{mode} lost the verified owning move stage\n"
        )
        return False
    return True


def validate_comma(generated: str, optimization: str, mode: str) -> bool:
    selected = ("_use_plain", "_use_comma", "CommaFlag::CommaFlag")
    for spelling in selected:
        if MARKER not in body_containing(generated, spelling):
            sys.stderr.write(
                f"{optimization}/{mode} did not emit comma fixture "
                f"body {spelling} from general MIR\n"
            )
            return False
    if MARKER not in body_containing(
        generated, "void CommaFlag::__gti_lifecycle_cleanup_"
    ):
        sys.stderr.write(
            f"{optimization}/{mode} did not emit CommaFlag cleanup from "
            "general MIR\n"
        )
        return False
    if "owned-lifecycle-call-v1" in generated:
        sys.stderr.write(
            f"{optimization}/{mode} revived the retired lifecycle route\n"
        )
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mir_backend_owned_lifecycle_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    comma_source = source.with_name(
        "mir_backend_owned_lifecycle_comma_near_miss.gti"
    )
    with tempfile.TemporaryDirectory(prefix="gti-mir-owned-lifecycle-") as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, _native in native_modes():
            executable = root / f"owned-{mode}"
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

            if is_cpp_mode(flags):
                emitted = root / f"owned-{mode}.cpp"
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
                if not validate_family(
                    emitted.read_text(encoding="utf8"), optimization_level(flags), mode
                ):
                    return 1

            comma_executable = root / f"comma-{mode}"
            comma_built = run(
                [
                    str(compiler),
                    str(comma_source),
                    *flags,
                    "-o",
                    str(comma_executable),
                ]
            )
            if comma_built.returncode != 0:
                return fail(comma_built)
            comma_executed = run([str(comma_executable)])
            if comma_executed.returncode != 0:
                return fail(comma_executed)

            if is_cpp_mode(flags):
                comma_emitted = root / f"comma-{mode}.cpp"
                comma_emission = run(
                    [
                        str(compiler),
                        str(comma_source),
                        *flags,
                        "--emit-cpp",
                        "-o",
                        str(comma_emitted),
                    ]
                )
                if comma_emission.returncode != 0:
                    return fail(comma_emission)
                if not validate_comma(
                    comma_emitted.read_text(encoding="utf8"),
                    optimization_level(flags), mode
                ):
                    return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
