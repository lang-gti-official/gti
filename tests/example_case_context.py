"""Authoritative source/profile/argument context of the existing MIR census."""

from pathlib import Path
import subprocess

from llvm_case_context import CaseContext, source_digest


CONCURRENT_EXAMPLES = frozenset({
    "066-managed-threads.gti", "076-scalar-atomics.gti", "175-thread-local-storage.gti",
})
CENSUS_TIMEOUT = 180
# Owner-approved LLVM-only wrapping operations are used by source shared owners.
LLVM_ONLY_EXAMPLES = frozenset({
    "066-managed-threads.gti", "098-shared-ownership.gti",
    "178-wrapping-atomic-counters.gti", "179-weak-ownership.gti",
    "181-general-object-layout.gti",
})


def require_cpp_limitation(gti, source, output, env, optimization="O1", standard="c++23"):
    limitation = ("general sizeof/alignof" if source.name == "181-general-object-layout.gti"
                  else "wrapping atomic read-modify-write")
    output.write_bytes(b"preserved unsupported output")
    result = subprocess.run(
        [str(gti), str(source), "--std", standard, "-" + optimization,
         *(["--execution-profile", "concurrent"] if source.name in CONCURRENT_EXAMPLES else []),
         "-o", str(output)], capture_output=True, text=True, env=env, timeout=CENSUS_TIMEOUT)
    assert (result.returncode == 65 and "GTI-B0003" in result.stderr
            and limitation in result.stderr
            and output.read_bytes() == b"preserved unsupported output"), result
    print(f"RECORDED C++ LIMITATION {source.name}/{standard}/{optimization}: {limitation}", flush=True)



def cases(root: Path) -> tuple[CaseContext, ...]:
    return tuple(CaseContext(
        provider="shipped-examples", identity=source.relative_to(root).as_posix(),
        source=source.relative_to(root).as_posix(), source_sha256=source_digest(source),
        execution_profile=("concurrent" if source.name in CONCURRENT_EXAMPLES
                           else "single-threaded"),
        loading="driver-default-prelude-with-stdlib-path-setdefault",
        observation=("sealed-lowered-census-with-cpp-rejection"
                     if source.name in LLVM_ONLY_EXAMPLES else "mir-census-emit-cpp"),
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
    native_only = Path(case.source).name in LLVM_ONLY_EXAMPLES
    return {
        "case": case.record(), "cwd": "repository-root", "timeout_seconds": CENSUS_TIMEOUT,
        "stdlib_environment": {"name": "GTI_STDLIB_PATH", "operation": "setdefault",
                               "repository_default": "stdlib"},
        "compile_observation": ({"mode": "sealed-lowered-program-inventory", "returncode": 0,
                                 "output_required": True, "oracle": "reviewed MIR body census",
                                 "cpp_rejection": ("GTI-B0003: general sizeof/alignof"
                                                   if Path(case.source).name == "181-general-object-layout.gti"
                                                   else "GTI-B0003: wrapping atomic read-modify-write")}
                                if native_only else
                                {"mode": "--emit-cpp", "returncode": 0,
                                 "output_required": True, "oracle": "reviewed MIR body census"}),
    }
