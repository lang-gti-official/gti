#!/usr/bin/env python3
"""Run optional, high-coverage local checks against the GTI language contract."""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
AUDIT_ROOT = REPOSITORY_ROOT / "tests" / "local-language-audit"
MANIFEST_PATH = AUDIT_ROOT / "cases.json"
EXAMPLES_ROOT = REPOSITORY_ROOT / "examples"
COMPARISON_RUNNER = EXAMPLES_ROOT / "gti-vs-cpp" / "verify.py"

INTERNAL_FAILURE_MARKERS = (
    "GTI-B0001",
    "internal compiler error",
    "assertion failed",
    "segmentation fault",
    "stack trace:",
)


class AuditFailure(RuntimeError):
    """A deterministic audit expectation was not satisfied."""


def run_command(
    command: Sequence[str],
    *,
    timeout: int,
    verbose: bool,
) -> subprocess.CompletedProcess[str]:
    if verbose:
        print("+ " + " ".join(command))
    try:
        return subprocess.run(
            list(command),
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise AuditFailure(
            f"command timed out after {timeout} seconds: {' '.join(command)}"
        ) from error


def combined_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def command_failure(
    context: str,
    command: Sequence[str],
    result: subprocess.CompletedProcess[str],
) -> AuditFailure:
    return AuditFailure(
        f"{context}\n"
        f"command: {' '.join(command)}\n"
        f"exit: {result.returncode}\n"
        f"--- stdout ---\n{result.stdout}"
        f"--- stderr ---\n{result.stderr}"
    )


def assert_no_internal_failure(
    context: str, result: subprocess.CompletedProcess[str]
) -> None:
    output = combined_output(result).casefold()
    if result.returncode < 0:
        raise AuditFailure(
            f"{context} terminated by signal {-result.returncode}\n{output}"
        )
    for marker in INTERNAL_FAILURE_MARKERS:
        if marker.casefold() in output:
            raise AuditFailure(f"{context} exposed {marker!r}\n{output}")


def require_fragments(context: str, text: str, fragments: Sequence[str]) -> None:
    folded = text.casefold()
    missing = [fragment for fragment in fragments if fragment.casefold() not in folded]
    if missing:
        raise AuditFailure(
            f"{context} is missing expected fragment(s): {', '.join(missing)}\n"
            f"--- output ---\n{text}"
        )


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
    raise AuditFailure(f"could not find {label}: {candidate}")


def load_manifest() -> list[dict[str, Any]]:
    with MANIFEST_PATH.open("r", encoding="utf-8") as manifest_file:
        manifest = json.load(manifest_file)
    cases = manifest.get("cases")
    if manifest.get("schema") != 1 or not isinstance(cases, list):
        raise AuditFailure(f"unsupported audit manifest: {MANIFEST_PATH}")
    identifiers = [case.get("id") for case in cases]
    if any(not isinstance(identifier, str) for identifier in identifiers):
        raise AuditFailure("every audit case requires a string id")
    if len(identifiers) != len(set(identifiers)):
        raise AuditFailure("audit case ids must be unique")
    return cases


def matrix(full: bool) -> list[tuple[str, str]]:
    configurations = [("O0", "c++23"), ("O3", "c++23")]
    if full:
        configurations.extend([("O0", "c++20"), ("O3", "c++20")])
    return configurations


def compile_command(
    gti: str,
    cxx: str,
    source: Path,
    output: Path,
    optimization: str,
    standard: str,
    *,
    emit_cpp: bool = False,
) -> list[str]:
    command = [
        gti,
        str(source),
        f"-{optimization}",
        "--std",
        standard,
        "-o",
        str(output),
    ]
    if emit_cpp:
        command.append("--emit-cpp")
    else:
        command.extend(["--cxx", cxx])
    return command


def compile_successfully(
    context: str,
    command: list[str],
    *,
    timeout: int,
    verbose: bool,
) -> subprocess.CompletedProcess[str]:
    result = run_command(command, timeout=timeout, verbose=verbose)
    assert_no_internal_failure(context, result)
    if result.returncode != 0:
        raise command_failure(f"{context} failed to compile", command, result)
    return result


def run_executable(
    context: str,
    executable: Path,
    *,
    timeout: int,
    verbose: bool,
) -> subprocess.CompletedProcess[str]:
    command = [str(executable)]
    return run_command(command, timeout=timeout, verbose=verbose)


def verify_emission(
    case: dict[str, Any],
    source: Path,
    gti: str,
    cxx: str,
    output_root: Path,
    *,
    timeout: int,
    verbose: bool,
) -> None:
    emitted: list[str] = []
    for repetition in range(2):
        destination = output_root / f"{case['id']}-emit-{repetition}.cpp"
        command = compile_command(
            gti, cxx, source, destination, "O0", "c++23", emit_cpp=True
        )
        compile_successfully(
            f"{case['id']} emission {repetition + 1}",
            command,
            timeout=timeout,
            verbose=verbose,
        )
        if not destination.is_file():
            raise AuditFailure(f"{case['id']} emitted no C++ artifact")
        emitted.append(destination.read_text(encoding="utf-8"))
    if emitted[0] != emitted[1]:
        raise AuditFailure(f"{case['id']} generated C++ is nondeterministic")
    require_fragments(
        f"{case['id']} generated C++",
        emitted[0],
        case.get("emit_contains", []),
    )
    forbidden = [
        fragment
        for fragment in case.get("emit_excludes", [])
        if fragment.casefold() in emitted[0].casefold()
    ]
    if forbidden:
        raise AuditFailure(
            f"{case['id']} generated C++ contains forbidden fragment(s): "
            + ", ".join(forbidden)
        )


def verify_contract_cases(
    cases: list[dict[str, Any]],
    gti: str,
    cxx: str,
    output_root: Path,
    *,
    full: bool,
    timeout: int,
    verbose: bool,
) -> int:
    for case in cases:
        case_id = case["id"]
        source = AUDIT_ROOT / case["source"]
        if not source.is_file():
            raise AuditFailure(f"{case_id} source does not exist: {source}")
        kind = case.get("kind")

        if kind == "diagnostic":
            destination = output_root / f"{case_id}.cpp"
            command = compile_command(
                gti, cxx, source, destination, "O0", "c++23", emit_cpp=True
            )
            result = run_command(command, timeout=timeout, verbose=verbose)
            assert_no_internal_failure(case_id, result)
            if result.returncode == 0:
                raise AuditFailure(f"{case_id} unexpectedly compiled")
            output = combined_output(result)
            require_fragments(case_id, output, case.get("contains", []))
            require_fragments(case_id, output, case.get("codes", []))
            print(f"PASS contract {case_id}")
            continue

        if kind not in {"execution", "runtime_failure"}:
            raise AuditFailure(f"{case_id} has unknown case kind {kind!r}")

        baseline: tuple[int, str, str] | None = None
        for optimization, standard in matrix(full):
            variant = f"{optimization}-{standard}"
            executable = output_root / f"{case_id}-{variant}"
            command = compile_command(
                gti, cxx, source, executable, optimization, standard
            )
            compile_successfully(
                f"{case_id} {variant}",
                command,
                timeout=timeout,
                verbose=verbose,
            )
            result = run_executable(
                f"{case_id} {variant}",
                executable,
                timeout=timeout,
                verbose=verbose,
            )
            snapshot = (result.returncode, result.stdout, result.stderr)
            if baseline is None:
                baseline = snapshot
            elif snapshot != baseline:
                raise AuditFailure(
                    f"{case_id} output drifted for {variant}\n"
                    f"baseline: {baseline!r}\nactual: {snapshot!r}"
                )

            if kind == "execution":
                expected = (
                    int(case.get("exit_code", 0)),
                    case.get("stdout", ""),
                    case.get("stderr", ""),
                )
                if snapshot != expected:
                    raise AuditFailure(
                        f"{case_id} {variant} output changed\n"
                        f"expected: {expected!r}\nactual: {snapshot!r}"
                    )
            else:
                if result.returncode == 0:
                    raise AuditFailure(
                        f"{case_id} {variant} unexpectedly completed successfully"
                    )
                require_fragments(
                    f"{case_id} {variant}",
                    combined_output(result),
                    case.get("contains", []),
                )

        verify_emission(
            case,
            source,
            gti,
            cxx,
            output_root,
            timeout=timeout,
            verbose=verbose,
        )
        print(f"PASS contract {case_id}")
    return len(cases)


def generated_program(index: int, seed: int) -> str:
    generator = random.Random(seed + index * 7919)
    values = [generator.randint(1, 20) for _ in range(4)]
    iterations = generator.randint(1, 8)
    increment = generator.randint(1, 7)
    multiplier = generator.randint(2, 5)
    subtraction = generator.randint(0, 12)
    expected = (sum(values) + iterations * increment) * multiplier - subtraction
    literal_values = ", ".join(str(value) for value in values)
    return f"""int compute() {{
  int values[4] = {{{literal_values}}};
  mut int total = 0;
  for (mut int index = 0; index < 4; index++) {{
    total += values[index];
  }}
  for (mut int index = 0; index < {iterations}; index++) {{
    total += {increment};
  }}
  total = total * {multiplier};
  total -= {subtraction};
  return total;
}}

int main() {{
  if (compute() == {expected} and {expected} > 0) {{
    return 0;
  }}
  return 1;
}}
"""


def verify_generated_programs(
    count: int,
    seed: int,
    gti: str,
    cxx: str,
    output_root: Path,
    *,
    full: bool,
    timeout: int,
    verbose: bool,
) -> int:
    source_root = output_root / "generated"
    source_root.mkdir()
    for index in range(count):
        source = source_root / f"generated-{index:03d}.gti"
        source.write_text(generated_program(index, seed), encoding="utf-8")
        baseline: tuple[int, str, str] | None = None
        for optimization, standard in matrix(full):
            variant = f"{optimization}-{standard}"
            executable = source_root / f"generated-{index:03d}-{variant}"
            command = compile_command(
                gti, cxx, source, executable, optimization, standard
            )
            compile_successfully(
                f"generated program {index} {variant}",
                command,
                timeout=timeout,
                verbose=verbose,
            )
            result = run_executable(
                f"generated program {index} {variant}",
                executable,
                timeout=timeout,
                verbose=verbose,
            )
            snapshot = (result.returncode, result.stdout, result.stderr)
            if snapshot != (0, "", ""):
                raise AuditFailure(
                    f"generated program {index} {variant} failed: {snapshot!r}\n"
                    f"--- source ---\n{source.read_text(encoding='utf-8')}"
                )
            if baseline is None:
                baseline = snapshot
            elif snapshot != baseline:
                raise AuditFailure(
                    f"generated program {index} drifted for {variant}"
                )
    print(f"PASS generated semantic programs ({count}, seed {seed})")
    return count


def mutated_source(source: str, generator: random.Random, index: int) -> str:
    if not source:
        return "int main() { return 0; }"
    strategy = index % 5
    position = generator.randrange(len(source))
    if strategy == 0:
        return source[:position] + source[position + 1 :]
    if strategy == 1:
        return source[: max(1, position)]
    if strategy == 2:
        replacement = generator.choice(["{", "}", ";", "(", ")", ","])
        return source[:position] + replacement + source[position + 1 :]
    if strategy == 3:
        insertion = generator.choice(
            [" __gti_forbidden ", " override ", " mut ", " return ", " ; "]
        )
        return source[:position] + insertion + source[position:]
    width = min(generator.randint(1, 12), len(source) - position)
    fragment = source[position : position + width]
    return source[:position] + fragment + source[position:]


def verify_mutations(
    count: int,
    seed: int,
    gti: str,
    cxx: str,
    output_root: Path,
    *,
    timeout: int,
    verbose: bool,
) -> int:
    seed_paths = [
        AUDIT_ROOT / "execution" / "short-circuit.gti",
        AUDIT_ROOT / "execution" / "virtual-dispatch.gti",
        AUDIT_ROOT / "execution" / "reverse-drop-order.gti",
    ]
    seeds = [path.read_text(encoding="utf-8") for path in seed_paths]
    generator = random.Random(seed ^ 0x475449)
    mutation_root = output_root / "mutations"
    mutation_root.mkdir()
    accepted = 0
    rejected = 0

    for index in range(count):
        source_text = mutated_source(seeds[index % len(seeds)], generator, index)
        source = mutation_root / f"mutation-{index:03d}.gti"
        emitted = mutation_root / f"mutation-{index:03d}.cpp"
        source.write_text(source_text, encoding="utf-8")
        command = compile_command(
            gti, cxx, source, emitted, "O0", "c++23", emit_cpp=True
        )
        result = run_command(command, timeout=timeout, verbose=verbose)
        assert_no_internal_failure(f"mutation {index}", result)
        if result.returncode == 0:
            accepted += 1
            if not emitted.is_file() or emitted.stat().st_size == 0:
                raise AuditFailure(
                    f"mutation {index} succeeded without emitted C++\n"
                    f"--- source ---\n{source_text}"
                )
            continue
        rejected += 1
        output = combined_output(result)
        if "error[GTI-" not in output and "gti:" not in output:
            raise command_failure(
                f"mutation {index} failed without a structured diagnostic",
                command,
                result,
            )

    print(
        f"PASS malformed-source mutations ({count}: "
        f"{accepted} accepted, {rejected} rejected; seed {seed})"
    )
    return count


def numbered_examples() -> list[Path]:
    sources = sorted(EXAMPLES_ROOT.glob("[0-9][0-9]-*.gti"))
    module_entry = EXAMPLES_ROOT / "10-modules" / "main.gti"
    if module_entry.is_file():
        sources.append(module_entry)
    return sources


def verify_public_examples(
    gti: str,
    cxx: str,
    output_root: Path,
    *,
    timeout: int,
    verbose: bool,
) -> int:
    sources = numbered_examples()
    for index, source in enumerate(sources):
        baseline: tuple[int, str, str] | None = None
        for optimization, standard in matrix(True):
            variant = f"{optimization}-{standard}"
            executable = output_root / f"example-{index:03d}-{variant}"
            command = compile_command(
                gti, cxx, source, executable, optimization, standard
            )
            compile_successfully(
                f"public example {source.relative_to(REPOSITORY_ROOT)} {variant}",
                command,
                timeout=timeout,
                verbose=verbose,
            )
            result = run_executable(
                f"public example {source.name} {variant}",
                executable,
                timeout=timeout,
                verbose=verbose,
            )
            snapshot = (result.returncode, result.stdout, result.stderr)
            if result.returncode != 0:
                raise AuditFailure(
                    f"public example {source} failed for {variant}: {snapshot!r}"
                )
            if baseline is None:
                baseline = snapshot
            elif snapshot != baseline:
                raise AuditFailure(
                    f"public example {source} drifted for {variant}\n"
                    f"baseline: {baseline!r}\nactual: {snapshot!r}"
                )
    print(f"PASS public example matrix ({len(sources)} examples, 4 variants each)")
    return len(sources)


def verify_comparison_showcase(
    gti: str,
    cxx: str,
    *,
    timeout: int,
    verbose: bool,
) -> None:
    if not COMPARISON_RUNNER.is_file():
        raise AuditFailure(f"comparison runner is missing: {COMPARISON_RUNNER}")
    command = [
        sys.executable,
        str(COMPARISON_RUNNER),
        "--gti",
        gti,
        "--cxx",
        cxx,
    ]
    result = run_command(command, timeout=max(timeout, 120), verbose=verbose)
    assert_no_internal_failure("GTI versus C++ comparison showcase", result)
    if result.returncode != 0:
        raise command_failure(
            "GTI versus C++ comparison showcase failed", command, result
        )
    print("PASS paired GTI/C++ comparison showcase")


def verify_workflow_isolation() -> None:
    forbidden_references = (
        "local_language_audit.py",
        "local-language-audit",
    )
    checked = [
        REPOSITORY_ROOT / "CMakeLists.txt",
        REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml",
        REPOSITORY_ROOT / ".github" / "workflows" / "release.yml",
    ]
    for path in checked:
        text = path.read_text(encoding="utf-8")
        found = [reference for reference in forbidden_references if reference in text]
        if found:
            raise AuditFailure(
                f"optional local audit is wired into {path}: {', '.join(found)}"
            )
    print("PASS optional audit is absent from CMake, CI, and release workflows")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run optional deep GTI language-contract checks."
    )
    parser.add_argument("--gti", help="path to the GTI compiler")
    parser.add_argument("--cxx", help="path or command name for the C++ compiler")
    parser.add_argument(
        "--full",
        action="store_true",
        help="include C++20 parity, all public examples, and wider searches",
    )
    parser.add_argument("--seed", type=int, default=20260809)
    parser.add_argument("--generated", type=int)
    parser.add_argument("--mutations", type=int)
    parser.add_argument("--skip-generated", action="store_true")
    parser.add_argument("--skip-mutations", action="store_true")
    parser.add_argument("--skip-comparisons", action="store_true")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.timeout <= 0:
            raise AuditFailure("--timeout must be positive")
        generated_count = (
            arguments.generated
            if arguments.generated is not None
            else (24 if arguments.full else 8)
        )
        mutation_count = (
            arguments.mutations
            if arguments.mutations is not None
            else (100 if arguments.full else 24)
        )
        if generated_count < 0 or mutation_count < 0:
            raise AuditFailure("--generated and --mutations cannot be negative")

        gti = resolve_tool(
            arguments.gti, REPOSITORY_ROOT / "build" / "gti", "GTI compiler"
        )
        cxx_fallback = os.environ.get("GTI_CXX") or os.environ.get("CXX") or "c++"
        cxx = resolve_tool(arguments.cxx, cxx_fallback, "C++ compiler")
        cases = load_manifest()

        print(
            "Running full optional language audit..."
            if arguments.full
            else "Running quick optional language audit..."
        )
        verify_workflow_isolation()
        with tempfile.TemporaryDirectory(prefix="gti-local-language-audit-") as temp:
            output_root = Path(temp)
            verify_contract_cases(
                cases,
                gti,
                cxx,
                output_root,
                full=arguments.full,
                timeout=arguments.timeout,
                verbose=arguments.verbose,
            )
            if not arguments.skip_generated and generated_count:
                verify_generated_programs(
                    generated_count,
                    arguments.seed,
                    gti,
                    cxx,
                    output_root,
                    full=arguments.full,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
            if not arguments.skip_mutations and mutation_count:
                verify_mutations(
                    mutation_count,
                    arguments.seed,
                    gti,
                    cxx,
                    output_root,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
            if arguments.full:
                verify_public_examples(
                    gti,
                    cxx,
                    output_root,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
            if not arguments.skip_comparisons:
                verify_comparison_showcase(
                    gti,
                    cxx,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
        print("Optional GTI language audit passed.")
        return 0
    except (AuditFailure, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
