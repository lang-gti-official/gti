#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


MARKER = "// GTI verified-MIR body: owned-lifecycle-call-v1"


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


def validate_family(generated: str, optimization: str, standard: str) -> bool:
    if generated.count(MARKER) != 4:
        sys.stderr.write(
            f"{optimization}/{standard} did not select exactly two free "
            "functions, one constructor, and one destructor from the verified "
            "owned-lifecycle MIR family\n"
        )
        return False
    selected = ("_consume(", "_run_scopes(", "ScopeFlag::ScopeFlag(", "::~ScopeFlag(")
    for spelling in selected:
        if MARKER not in body_containing(generated, spelling):
            sys.stderr.write(
                f"{optimization}/{standard} did not select {spelling} from MIR\n"
            )
            return False
    compatibility = ("_compatibility_default_field(", "_compatibility_checked(")
    for spelling in compatibility:
        if MARKER in body_containing(generated, spelling):
            sys.stderr.write(
                f"{optimization}/{standard} selected ineligible {spelling}\n"
            )
            return False
    if "std::move" not in body_containing(generated, "_run_scopes("):
        sys.stderr.write(
            f"{optimization}/{standard} lost the verified owning move stage\n"
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
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"owned-{optimization}-{standard}"
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
                    return fail(built)
                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(executed)

                emitted = root / f"owned-{optimization}-{standard}.cpp"
                emission = run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "--emit-cpp",
                        "-o",
                        str(emitted),
                    ]
                )
                if emission.returncode != 0:
                    return fail(emission)
                if not validate_family(
                    emitted.read_text(encoding="utf8"), optimization, standard
                ):
                    return 1

                comma_executable = root / f"comma-{optimization}-{standard}"
                comma_built = run(
                    [
                        str(compiler),
                        str(comma_source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(comma_executable),
                    ]
                )
                if comma_built.returncode != 0:
                    return fail(comma_built)
                comma_executed = run([str(comma_executable)])
                if comma_executed.returncode != 0:
                    return fail(comma_executed)
                comma_emitted = root / f"comma-{optimization}-{standard}.cpp"
                comma_emission = run(
                    [
                        str(compiler),
                        str(comma_source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "--emit-cpp",
                        "-o",
                        str(comma_emitted),
                    ]
                )
                if comma_emission.returncode != 0:
                    return fail(comma_emission)
                if MARKER in comma_emitted.read_text(encoding="utf8"):
                    sys.stderr.write(
                        f"{optimization}/{standard} did not atomically demote "
                        "the comma function/constructor/destructor component\n"
                    )
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
