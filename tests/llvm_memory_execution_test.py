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
    "typed-relocation": "object_relocation.gti",
    "structural-relocation": "structural_relocation.gti",
    "relocation-composition": "llvm_memory_relocation_composition.gti",
}
EXPECTED_STDOUT = {
    "typed-relocation": "payload\n24\n23\n",
    "structural-relocation": "71\n82\n81\nrelocation passed\n",
}
# C++'s representation vocabulary rejects construct_at forwarding a concrete
# closure into a generic wrapper. This is not a native fallback or a parity
# pass: the fixture independently checks values and exactly-once destruction.
NATIVE_ONLY = {"relocation-composition"}
INLINE_CASES = {
    # Restoring swap, exchange and indexed swap relocate between disjoint
    # initialized places without copying or running cleanup. Each live object
    # must be destroyed exactly once afterwards, whole arrays swap as one
    # object, and swapping an element with itself must change nothing.
    "restoring-exchanges": r'''
#include <std/array>
#include <std/string>
#include <std/utility>

mut int destroyed = 0;

struct Asset {
  std::string name;
  int id;
  Asset(std::string name, int id) : name(std::move(name)), id(id) {}
  Asset(Asset& other) = delete;
  ~Asset() { destroyed += 1; }
};

int exchanges() {
  mut Asset first = Asset(std::string("alpha"), 1);
  mut Asset second = Asset(std::string("beta"), 2);
  std::swap(first, second);
  if (first.id != 2 || second.id != 1) { return 1; }
  if (first.name != "beta" || second.name != "alpha") { return 2; }
  if (destroyed != 0) { return 3; }
  Asset old = std::exchange(first, Asset(std::string("gamma"), 3));
  if (first.id != 3 || first.name != "gamma") { return 4; }
  if (old.id != 2 || old.name != "beta") { return 5; }
  if (destroyed != 0) { return 6; }
  return 0;
}

int main() {
  int status = exchanges();
  if (status != 0) { return status; }
  // first, second and old are each destroyed exactly once.
  if (destroyed != 3) { return 7; }

  mut std::array<std::string, 3> words = std::array<std::string, 3>(
      {std::string("one"), std::string("two"), std::string("three")});
  words.swap_elements(std::size_t(0), std::size_t(2));
  if (words[0] != "three" || words[2] != "one") { return 8; }
  words.swap_elements(std::size_t(1), std::size_t(1));
  if (words[1] != "two") { return 9; }

  mut std::array<std::string, 2> day = std::array<std::string, 2>(
      {std::string("dawn"), std::string("noon")});
  mut std::array<std::string, 2> night = std::array<std::string, 2>(
      {std::string("dusk"), std::string("midnight")});
  day.swap(night);
  if (day[0] != "dusk" || day[1] != "midnight") { return 10; }
  if (night[0] != "dawn" || night[1] != "noon") { return 11; }

  mut int counter = 5;
  int previous = std::exchange(counter, 8);
  if (previous != 5 || counter != 8) { return 12; }
  return 0;
}
''',
    # Typed storage of hierarchy elements: by-value push relocates each
    # argument into raw element storage, growth relocates elements with their
    # dispatch headers and owned text, element views dispatch through raw
    # references, and destruction runs each exact recipe once in reverse.
    "hierarchy-element-storage": r'''
#include <std/string>
#include <std/vector>

mut int destroyed = 0;
mut int trace = 0;

interface Shape { int area(); };
interface Marker { int mark(); };

class Square : public Marker, public Shape {
  int side;
  std::string label;
public:
  Square(int side) : side(side), label(std::string("sq")) {}
  ~Square() {
    destroyed += 1;
    trace = trace * 10 + this.side;
  }
  int mark() override { return this.side; }
  int area() override { return this.side * this.side + int(this.label.size()); }
};

int total(std::vector<Square>& shapes) {
  mut int sum = 0;
  for (mut std::size_t index = 0; index < shapes.size(); index++) {
    Shape& view = shapes[index];
    sum += view.area();
  }
  return sum;
}

int main() {
  {
    mut std::vector<Square> shapes{};
    for (mut int side = 1; side <= 5; side++) {
      shapes.push_back(Square(side));
    }
    if (total(shapes) != 65) { return 1; }
    // Relocation during growth ends no element lifetime.
    if (destroyed != 0) { return 2; }
  }
  return destroyed == 5 && trace == 54321 ? 0 : 3;
}
''',
    "address-conversion": r'''
struct Reading { int value = 42;  public: Reading() = default; };
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


def check_structural_failures(binary: Path, root: Path, env: dict[str, str]):
    lines = (root / "stdlib/std/memory.gti").read_text(encoding="utf-8").splitlines()
    def region(start: str, end: str):
        owner = next(index for index, line in enumerate(lines) if "class sparse_slots<" in line)
        begin = next(index for index in range(owner, len(lines)) if start in lines[index])
        finish = next(index for index in range(begin + 1, len(lines)) if end in lines[index])
        return range(begin, finish)

    read = region("T& read(uint64_t index)", "mut T& read_mut(uint64_t index)")
    relocation = region("void relocate_to(mut sparse_slots<T>& destination", "void shift_right(")
    cases = (
        (1, "require_relocation_capacity(count <= this.slots", "relocation_capacity", ""),
        (2, "require_relocation_destination(!destination.initialized(index))",
         "occupied_relocation_destination", ""),
        (3, "require_relocation_source(this.initialized(index))", "invalid_relocation_source", ""),
        (4, "require_live_storage(this.initialized(index))", "uninitialized_access", ""),
        (5, "require_relocation_destination(!destination.initialized(index))",
         "occupied_relocation_destination", "201\n104\n102\n101\n"),
        (6, "require_relocation_source(this.initialized(index))",
         "invalid_relocation_source", "104\n102\n101\n"),
    )
    for count, source, detail, stdout in cases:
        scope = read if detail == "uninitialized_access" else relocation
        matches = [index + 1 for index in scope if source in lines[index]]
        if len(matches) != 1:
            raise AssertionError(f"ambiguous structural failure source: {source}: {matches}")
        stderr = (f"<std/memory>:{matches[0]}: runtime error[GTI-R0010]: "
                  f"invalid_storage_state in {detail}\n")
        result = subprocess.run([str(binary), *(["case"] * count)], cwd=root,
                                env=env, capture_output=True, text=True, timeout=15)
        if (result.returncode, result.stdout, result.stderr) != (70, stdout, stderr):
            raise AssertionError(f"structural failure {count}: {result}")


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
                if name in NATIVE_ONLY and endpoint.backend == "cpp":
                    continue
                result = execute(gti, root, work / f"{name}-{endpoint}", source, endpoint, env)
                if result.error or result.observable != (0, EXPECTED_STDOUT.get(name, ""), ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                if name == "structural-relocation":
                    binary = work / f"{name}-{endpoint}" / (
                        "program.exe" if os.name == "nt" else "program")
                    check_structural_failures(binary, root, env)
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"memory observable drift: {name}: {observed}")
            coverage = ("native LLVM O0-O3 source-contract oracle (known C++ representation gap)"
                        if name in NATIVE_ONLY else "C++20/23O3 and native LLVM O0-O3")
            print(f"{name}: {coverage} passed", flush=True)
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
