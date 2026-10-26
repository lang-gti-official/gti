#!/usr/bin/env python3
"""Exact memory/lifetime execution, without a generated-C++ native fallback."""

import os
from pathlib import Path
import sys
import subprocess
import tempfile
import json

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


CASES = {
    "layout": "llvm_memory_layout.gti",
    "zero-and-passive": "llvm_memory_zero_and_passive.gti",
    "vacant": "llvm_memory_vacant.gti",
    "guards": "llvm_memory_guards.gti",
    "lifetimes": "llvm_memory_lifetimes.gti",
    "value-lifetimes": "llvm_memory_value_lifetimes.gti",
    "provider": "llvm_memory_provider.gti",
    "empty-unique": "llvm_memory_empty_unique.gti",
    "unique": "llvm_memory_unique.gti",
    "empty-shared": "llvm_memory_empty_shared.gti",
    "padded-range": "llvm_memory_padded_range.gti",
    "shared": "llvm_memory_shared.gti",
}
INLINE_CASES = {
    "address-conversion": r'''
struct Reading { int value = 42; };
using Erased = void*;
using Typed = Reading*;

int main() {
  mut Reading owner{};
  Typed address = &owner;
  Erased erased = Erased(address);
  Typed restored = Typed(erased);
  return restored == address ? 0 : 1;
}
''',
}
REJECT = {"vacant-move": "llvm_memory_reject_vacant_move.gti"}
FAILURES = {"guard-cleanup": ("llvm_memory_guard_cleanup.gti", "GTI-R0014",
                              "Boolean-guard cleanup trace observed")}


def evidence_cases(root: Path) -> dict[str, str]:
    fixtures = root / "tests/fixtures"
    cases = {
        name: (fixtures / filename).read_text(encoding="utf-8")
        for name, filename in CASES.items()
    }
    cases.update(INLINE_CASES)
    cases.update({
        name: (fixtures / filename).read_text(encoding="utf-8")
        for name, (filename, _, _) in FAILURES.items()
    })
    return cases


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return {"memory-core": tuple(CASES) + tuple(INLINE_CASES) + tuple(FAILURES)}


def evidence_manifest(root: Path) -> dict:
    origins = {
        name: f"tests/fixtures/{filename}"
        for name, filename in CASES.items()
    }
    origins.update({
        name: f"tests/fixtures/{filename}"
        for name, (filename, _, _) in FAILURES.items()
    })
    return build_manifest(
        "llvm_memory_execution", evidence_cases(root), evidence_profiles(root),
        origins)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()),
                         sort_keys=True))
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected == "publication-rejection":
        # Component-only prerequisite check. The positive empty-unique program
        # remains in CASES and must execute after O09's publication composition.
        with tempfile.TemporaryDirectory(prefix="gti-llvm-memory-preflight-") as directory:
            for level in ("O0", "O1", "O2", "O3"):
                output = Path(directory) / f"publication-{level}"
                output.write_text("preserved", encoding="utf-8")
                result = subprocess.run(
                    [str(gti), str(root / "tests/fixtures/llvm_memory_empty_unique.gti"),
                     "--backend", "llvm", f"-{level}", "-o", str(output)],
                    cwd=root, capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "GTI-B0003" not in result.stderr
                        or "UnstagedConstructorFieldPublication" not in result.stderr
                        or output.read_text() != "preserved"):
                    raise AssertionError(f"publication preflight {level}: {result}")
        print("publication prerequisite: LLVM O0-O3 rejection preserved artifacts", flush=True)
        return
    if (selected and selected not in CASES and selected not in INLINE_CASES
            and selected not in REJECT and selected not in FAILURES):
        raise ValueError(f"unknown memory case: {selected}")
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", level) for level in ("O0", "O1", "O2", "O3")))
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-memory-") as directory:
        work = Path(directory)
        for name in (*CASES, *INLINE_CASES):
            if selected and selected != name:
                continue
            if name in INLINE_CASES:
                source = work / f"{name}.gti"
                source.write_text(INLINE_CASES[name], encoding="utf-8")
            else:
                source = root / "tests/fixtures" / CASES[name]
            observed = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"memory observable drift: {name}: {observed}")
            print(f"{name}: C++20/23O3 and native LLVM O0-O3 passed", flush=True)
        for name, (filename, code, marker) in FAILURES.items():
            if selected and selected != name:
                continue
            observed = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}",
                                 root / "tests/fixtures" / filename, endpoint, env)
                if (result.error or result.returncode != 70 or code not in result.stderr
                        or marker not in result.stderr or "wrong Boolean-guard" in result.stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"memory failure attribution drift: {name}: {observed}")
            print(f"{name}: source-attributed failure and exactly-once cleanup passed", flush=True)
        for name, filename in REJECT.items():
            if selected and selected != name:
                continue
            for backend in ("cpp", "llvm"):
                output = work / f"rejected-{name}-{backend}"
                output.write_text("preserved", encoding="utf-8")
                result = subprocess.run(
                    [str(gti), str(root / "tests/fixtures" / filename), "--backend", backend,
                     "-O3", "-o", str(output)], cwd=root, env=env,
                    capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "error[GTI-S" not in result.stderr
                        or "GTI-B" in result.stderr or output.read_text() != "preserved"):
                    raise AssertionError(f"{name} {backend}: {result}")
            print(f"{name}: source rejection and artifact preservation passed", flush=True)


if __name__ == "__main__":
    main()
