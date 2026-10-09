#!/usr/bin/env python3
"""Inline capturing lambda literals as generic-algorithm arguments."""

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


def validate_emission(generated: str, mode: str) -> bool:
    # MIR place IDs are body-local. Source-defined storage introduces more
    # bodies using the same slot numbers; they are not extra closure transfers.
    entries = list(re.finditer(
        r"^  bool __gti_entry__gti_mir_failure\([^\n]*\) \{\n"
        r"(?P<body>.*?)^  \}\n", generated, re.MULTILINE | re.DOTALL,
    ))
    if len(entries) != 1:
        sys.stderr.write(f"{mode} lost the exact entry failure body\n")
        return False
    generated = entries[0].group("body")
    stages = list(re.finditer(
        r"(?P<slot>__gti_mir_p_\d+)\.construct_from\(\[&\]\(\) \{ return "
        r"__gti_mir_closure_factory_(?P<identity>\d+)\((?P<capture>[^\n]+)\); \}\);",
        generated,
    ))
    if len(stages) != 3:
        sys.stderr.write(f"{mode} lost the three prepared closure constructions\n")
        return False
    for stage, algorithm, moved in zip(
        stages, ("count_if", "any_of", "all_of"), (False, True, False), strict=True
    ):
        slot, identity, capture = stage.group("slot", "identity", "capture")
        capture_pattern = (r"std::move\(__gti_mir_p_\d+\.get\(\)\)" if moved
                           else r"__gti_mir_p_\d+")
        argument = f"__gti_mir_capture_arg_{identity}_0"
        field = f"std::move({argument})" if moved else argument
        factory_return = f"return __gti_mir_closure_environment_{identity}{{{field}}};"
        calls = list(re.finditer(
            rf"^\s*__gti_mir_call_success_\d+ = [^\n]*_{algorithm}__gti_mir_failure"
            rf"\([^\n]*std::move\({re.escape(slot)}\.get\(\)\)[^\n]*;",
            generated, re.MULTILINE,
        ))
        if (not re.fullmatch(capture_pattern, capture)
                or generated.count(factory_return) != 1 or len(calls) != 1):
            sys.stderr.write(f"{mode} lost {algorithm}'s exact capture/argument identity\n")
            return False
        call = calls[0]
        retire = generated.find(f"{slot}.destroy();", call.end())
        if (not stage.end() < call.start() < retire
                or generated.count(f"{slot}.construct_from(") != 1
                or generated.count(f"std::move({slot}.get())") != 1
                or "goto " in generated[call.end():retire]
                or "if (" in generated[call.end():retire]):
            sys.stderr.write(f"{mode} lost {algorithm}'s once-only preparation and retirement\n")
            return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: inline_lambda_argument_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-inline-lambda-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            executable = root / f"inline-lambda-{optimization}"
            built = run(
                [str(compiler), str(source), f"-{optimization}", "-o", str(executable)]
            )
            if built.returncode != 0:
                return fail(f"build {optimization}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != "inline lambda arguments ok\n"
                or executed.stderr
            ):
                return fail(f"run {optimization}", executed)

            emitted = root / f"inline-lambda-{optimization}.cpp"
            emission = run(
                [
                    str(compiler),
                    str(source),
                    f"-{optimization}",
                    "--emit-cpp",
                    "-o",
                    str(emitted),
                ]
            )
            if emission.returncode != 0:
                return fail(f"emit {optimization}", emission)
            if not validate_emission(
                emitted.read_text(encoding="utf-8"), optimization
            ):
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
