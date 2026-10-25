"""Authoritative source/profile/argument context of the existing MIR census."""

from pathlib import Path

from llvm_case_context import CaseContext, source_digest


CONCURRENT_EXAMPLES = frozenset({
    "066-managed-threads.gti", "076-scalar-atomics.gti", "175-thread-local-storage.gti",
})
CENSUS_TIMEOUT = 180


def cases(root: Path) -> tuple[CaseContext, ...]:
    return tuple(CaseContext(
        provider="shipped-examples", identity=source.relative_to(root).as_posix(),
        source=source.relative_to(root).as_posix(), source_sha256=source_digest(source),
        execution_profile=("concurrent" if source.name in CONCURRENT_EXAMPLES
                           else "single-threaded"),
        loading="driver-default-prelude-with-stdlib-path-setdefault",
        observation="mir-census-emit-cpp",
        unresolved=("llvm-target-applicability", "native-execution-outcome",
                    "native-dependencies", "native-optimization-endpoints"),
    ) for source in sorted((root / "examples").glob("*.gti")))


def environment(root: Path, inherited: dict[str, str]) -> dict[str, str]:
    result = dict(inherited)
    result.setdefault("GTI_STDLIB_PATH", str(root / "stdlib"))
    return result


def census_command(case: CaseContext, root: Path, gti: Path, output: Path) -> list[str]:
    return [str(gti), "--emit-cpp",
            *(["--execution-profile", "concurrent"]
              if case.execution_profile == "concurrent" else []),
            str(root / case.source), "-o", str(output)]


def observation_context(case: CaseContext) -> dict:
    return {
        "case": case.record(), "cwd": "repository-root", "timeout_seconds": CENSUS_TIMEOUT,
        "stdlib_environment": {"name": "GTI_STDLIB_PATH", "operation": "setdefault",
                               "repository_default": "stdlib"},
        "compile_observation": {"mode": "--emit-cpp", "returncode": 0,
                                "output_required": True,
                                "oracle": "reviewed MIR body census"},
    }
