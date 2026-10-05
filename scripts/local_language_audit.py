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
from typing import Any, Callable, Literal, Sequence

from debug.typed_program_model import (
    TypedAuditCase,
    composition_case,
    generated_case,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
AUDIT_ROOT = REPOSITORY_ROOT / "tests" / "local-language-audit"
MANIFEST_PATH = AUDIT_ROOT / "cases.json"
EXAMPLES_ROOT = REPOSITORY_ROOT / "examples"

# Public examples normally use the safe single-threaded default. Examples that
# intentionally exercise a capability-gated execution profile declare that
# requirement here so the full audit validates them under their supported
# configuration instead of silently excluding them from the corpus.
PUBLIC_EXAMPLE_EXECUTION_PROFILES = {
    "66-managed-threads.gti": "concurrent",
    "76-scalar-atomics.gti": "concurrent",
}

INTERNAL_FAILURE_MARKERS = (
    "GTI-B0001",
    "internal compiler error",
    "assertion failed",
    "segmentation fault",
    "stack trace:",
)


class AuditFailure(RuntimeError):
    """A deterministic audit expectation was not satisfied."""


TypedMismatch = Literal["evaluator", "reference", "gti"]


def perturbed_snapshot(snapshot: tuple[int, str, str]) -> tuple[int, str, str]:
    """Return a deterministic, still-portable result distinct from snapshot."""

    return ((snapshot[0] + 1) % 126, snapshot[1], snapshot[2])


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
        # Preserve the executable basename. Resolving a clang++ symlink to
        # `clang` changes the driver's link mode and omits the C++ runtime.
        path = Path(candidate).expanduser().absolute()
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


def cxx_compile_command(
    cxx: str, source: Path, output: Path, optimization: str
) -> list[str]:
    return [
        cxx,
        "-std=c++23",
        f"-{optimization}",
        str(source),
        "-o",
        str(output),
    ]


def preserve_typed_failure(
    case: TypedAuditCase,
    case_root: Path,
    artifacts_root: Path,
    error: AuditFailure,
    gti: str,
    timeout: int,
    verbose: bool,
) -> Path:
    artifacts_root.mkdir(parents=True, exist_ok=True)
    destination = artifacts_root / f"typed-{case.seed}-{case.index:04d}"
    suffix = 1
    while destination.exists():
        destination = artifacts_root / f"typed-{case.seed}-{case.index:04d}-{suffix}"
        suffix += 1
    partial = destination.parent / f".{destination.name}.partial-{os.getpid()}"
    shutil.copytree(case_root, partial)
    (partial / "failure.json").write_text(
        json.dumps(
            {
                "schema": "gti-typed-audit-failure-v1",
                "error": str(error),
                "case": case.semantic_record(),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    original_source = partial / "original.gti"
    capture_command = [
        sys.executable,
        str(REPOSITORY_ROOT / "scripts/debug/capture_compiler_failure.py"),
        "capture",
        "--gti",
        gti,
        "--source",
        str(original_source),
        "--out",
        str(partial / "gti-debug-bundle"),
        "--timeout",
        str(timeout),
    ]
    captured = run_command(
        capture_command, timeout=max(timeout * 5, 30), verbose=verbose
    )
    (partial / "capture.stdout").write_text(captured.stdout, encoding="utf-8")
    (partial / "capture.stderr").write_text(captured.stderr, encoding="utf-8")
    os.replace(partial, destination)
    return destination


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
    artifacts_root: Path | None,
    injected_mismatch: TypedMismatch | None = None,
    case_factory: Callable[[int, int], TypedAuditCase] = generated_case,
    source_directory: str = "generated",
    description: str = "typed differential programs",
) -> int:
    source_root = output_root / source_directory
    source_root.mkdir()
    for index in range(count):
        case = case_factory(index, seed)
        case_root = source_root / f"case-{index:03d}"
        case_root.mkdir()
        variants = (
            ("original", case.original),
            (case.transformation, case.transformed),
        )
        for name, program in variants:
            (case_root / f"{name}.gti").write_text(
                program.emit("gti"), encoding="utf-8"
            )
            (case_root / f"{name}.cpp").write_text(
                program.emit("cpp"), encoding="utf-8"
            )
        (case_root / "semantic-case.json").write_text(
            case.semantic_json(), encoding="utf-8"
        )

        try:
            expected = (case.expected, "", "")
            if injected_mismatch == "evaluator":
                expected = perturbed_snapshot(expected)
            observed: dict[str, tuple[int, str, str]] = {}
            for name, _ in variants:
                cxx_source = case_root / f"{name}.cpp"
                for optimization in ("O0", "O3"):
                    variant = f"reference-{name}-{optimization}"
                    executable = case_root / variant
                    command = cxx_compile_command(
                        cxx, cxx_source, executable, optimization
                    )
                    compile_successfully(
                        f"typed case {index} {variant}",
                        command,
                        timeout=timeout,
                        verbose=verbose,
                    )
                    result = run_executable(
                        f"typed case {index} {variant}",
                        executable,
                        timeout=timeout,
                        verbose=verbose,
                    )
                    assert_no_internal_failure(f"typed case {index} {variant}", result)
                    snapshot = (result.returncode, result.stdout, result.stderr)
                    if (
                        injected_mismatch == "reference"
                        and name == "original"
                        and optimization == "O0"
                    ):
                        snapshot = perturbed_snapshot(snapshot)
                    observed[variant] = snapshot
                    if snapshot != expected:
                        raise AuditFailure(
                            f"typed case {index} independent C++ {variant} disagreed "
                            f"with the evaluator\nexpected: {expected!r}\n"
                            f"actual: {snapshot!r}"
                        )

                gti_source = case_root / f"{name}.gti"
                for optimization, standard in matrix(full):
                    variant = f"gti-{name}-{optimization}-{standard}"
                    executable = case_root / variant
                    command = compile_command(
                        gti, cxx, gti_source, executable, optimization, standard
                    )
                    compile_successfully(
                        f"typed case {index} {variant}",
                        command,
                        timeout=timeout,
                        verbose=verbose,
                    )
                    result = run_executable(
                        f"typed case {index} {variant}",
                        executable,
                        timeout=timeout,
                        verbose=verbose,
                    )
                    assert_no_internal_failure(f"typed case {index} {variant}", result)
                    snapshot = (result.returncode, result.stdout, result.stderr)
                    if (
                        injected_mismatch == "gti"
                        and name == "original"
                        and optimization == "O0"
                        and standard == "c++23"
                    ):
                        snapshot = perturbed_snapshot(snapshot)
                    observed[variant] = snapshot
                    if snapshot != expected:
                        raise AuditFailure(
                            f"typed case {index} {variant} disagreed with the evaluator\n"
                            f"expected: {expected!r}\nactual: {snapshot!r}"
                        )

            if len(set(observed.values())) != 1:
                raise AuditFailure(
                    f"typed case {index} optimization or metamorphic output drifted: "
                    f"{observed!r}"
                )
        except AuditFailure as error:
            if artifacts_root is not None:
                preserved = preserve_typed_failure(
                    case,
                    case_root,
                    artifacts_root,
                    error,
                    gti,
                    timeout,
                    verbose,
                )
                raise AuditFailure(f"{error}\npreserved: {preserved}") from error
            raise
    print(
        f"PASS {description} ({count}, seed {seed}; "
        "evaluator + C++ O0/O3 + GTI optimization matrix)"
    )
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
    sources = sorted(
        (source for source in EXAMPLES_ROOT.glob("[0-9]*-*.gti")
         if source.is_file() and source.name.partition("-")[0].isdigit()),
        key=lambda source: (int(source.name.partition("-")[0]), source.name),
    )
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
            execution_profile = PUBLIC_EXAMPLE_EXECUTION_PROFILES.get(source.name)
            if execution_profile is not None:
                command.extend(["--execution-profile", execution_profile])
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


def verify_workflow_isolation() -> None:
    forbidden_references = (
        "local_language_audit.py",
        "local-language-audit",
    )
    checked = [
        REPOSITORY_ROOT / "CMakeLists.txt",
        REPOSITORY_ROOT / ".gitea" / "workflows" / "ci.yml",
        REPOSITORY_ROOT / ".gitea" / "workflows" / "release.yml",
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
    parser.add_argument("--compositions", type=int)
    parser.add_argument("--mutations", type=int)
    parser.add_argument("--skip-generated", action="store_true")
    parser.add_argument("--skip-compositions", action="store_true")
    parser.add_argument("--skip-mutations", action="store_true")
    parser.add_argument(
        "--typed-only",
        action="store_true",
        help="run only the bounded typed differential program checks",
    )
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument(
        "--keep-failures",
        action="store_true",
        help="preserve failure evidence instead of deleting it with the "
        "temporary directory: the audit working tree, plus a generated "
        "mismatch and its GTI debug bundle",
    )
    parser.add_argument(
        "--artifacts-dir",
        help="directory to preserve failures under (implies --keep-failures)",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


class _KeepOnFailure:
    """A working-directory holder that survives failures.

    On clean exit the tree is deleted like a temporary directory; when the
    audit raises, the tree is left in place and `kept_path` names it so the
    failure can be captured and reduced instead of vanishing.
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.kept_path: Path | None = None

    def __enter__(self) -> str:
        return str(self.root)

    def __exit__(self, exc_type, exc, tb) -> None:
        if exc_type is None:
            shutil.rmtree(self.root, ignore_errors=True)
        else:
            self.kept_path = self.root


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
        composition_count = (
            arguments.compositions
            if arguments.compositions is not None
            else (4 if arguments.full else 2)
        )
        mutation_count = (
            arguments.mutations
            if arguments.mutations is not None
            else (100 if arguments.full else 24)
        )
        if generated_count < 0 or composition_count < 0 or mutation_count < 0:
            raise AuditFailure(
                "--generated, --compositions, and --mutations cannot be negative"
            )
        if (
            arguments.typed_only
            and arguments.skip_generated
            and arguments.skip_compositions
        ):
            raise AuditFailure(
                "--typed-only requires generated or composition checks"
            )
        artifacts_root = None
        if arguments.keep_failures or arguments.artifacts_dir:
            artifacts_root = Path(
                arguments.artifacts_dir or "gti-audit-failures"
            ).expanduser().resolve()

        gti = resolve_tool(
            arguments.gti, REPOSITORY_ROOT / "build" / "gti", "GTI compiler"
        )
        cxx_fallback = os.environ.get("GTI_CXX") or os.environ.get("CXX") or "c++"
        cxx = resolve_tool(arguments.cxx, cxx_fallback, "C++ compiler")
        cases = [] if arguments.typed_only else load_manifest()

        print(
            "Running full optional language audit..."
            if arguments.full
            else "Running quick optional language audit..."
        )
        verify_workflow_isolation()
        # With --keep-failures the working tree survives a failure so the
        # evidence can be captured and reduced (scripts/debug/); by default
        # it is a temporary directory, deleted either way.
        keep_failures = arguments.keep_failures or bool(arguments.artifacts_dir)
        if keep_failures:
            artifacts_parent = Path(arguments.artifacts_dir or ".").resolve()
            artifacts_parent.mkdir(parents=True, exist_ok=True)
            kept_root = Path(
                tempfile.mkdtemp(
                    prefix="gti-audit-failure-", dir=artifacts_parent
                )
            )
            keeper = _KeepOnFailure(kept_root)
        else:
            keeper = tempfile.TemporaryDirectory(
                prefix="gti-local-language-audit-"
            )
        with keeper as temp:
            output_root = Path(temp)
            if not arguments.typed_only:
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
                    artifacts_root=artifacts_root,
                )
            if not arguments.skip_compositions and composition_count:
                verify_generated_programs(
                    composition_count,
                    arguments.seed,
                    gti,
                    cxx,
                    output_root,
                    full=arguments.full,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                    artifacts_root=artifacts_root,
                    case_factory=composition_case,
                    source_directory="compositions",
                    description="typed composition programs",
                )
            if (
                not arguments.typed_only
                and not arguments.skip_mutations
                and mutation_count
            ):
                verify_mutations(
                    mutation_count,
                    arguments.seed,
                    gti,
                    cxx,
                    output_root,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
            if arguments.full and not arguments.typed_only:
                verify_public_examples(
                    gti,
                    cxx,
                    output_root,
                    timeout=arguments.timeout,
                    verbose=arguments.verbose,
                )
        print("Optional GTI language audit passed.")
        return 0
    except (AuditFailure, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        kept = getattr(locals().get("keeper"), "kept_path", None)
        if kept is not None:
            print(f"failing working tree preserved at: {kept}", file=sys.stderr)
            print(
                "capture a failing source from it with: python3 "
                "scripts/debug/capture_compiler_failure.py capture "
                "--gti <compiler> --source <file.gti> --out <bundle>",
                file=sys.stderr,
            )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
