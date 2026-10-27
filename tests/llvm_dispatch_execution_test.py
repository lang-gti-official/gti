#!/usr/bin/env python3
"""Execute the bounded static-local dispatch slice and reject object transfers.

The default cost fixture is correctness-only. Opt-in --measure EVIDENCE prepares
artifacts and stops; --measure-review REVIEW --measure-host-plan PLAN resumes
only after independent artifact review and an approved platform host window.
Neither path infers allocation counts from source shape or process RSS.
The verified constructor-state plan and explicit destructor IDs establish the
positive traces. Virtual observations during destruction/rollback remain
rejections pending an explicit backend-neutral phase contract. No C++ result
defines that missing contract, and no proposed phase trace is accepted here.
The nested mutable-reference chain is source/MIR-valid and native-required;
existing C++ text-vocabulary rejections are recorded, not counted as passes.
The isolated generated copy through a Loan also retains its precise C++20/23
rejections while requiring verified source/MIR and native O0-O3 execution.
"""

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

from backend_execution import Endpoint, execute
from llvm_case_context import CaseContext
from llvm_evidence_manifest import build_context_sidecar, build_manifest


ENDPOINTS = (
    Endpoint("cpp", "O3", "c++20"),
    Endpoint("cpp", "O3", "c++23"),
    Endpoint("llvm", "O0"),
    Endpoint("llvm", "O3"),
)

SUCCESS_FILES = {
    "static-dispatch": "tests/fixtures/llvm_dispatch_execution.gti",
    "fresh-hierarchy-transfer": "tests/fixtures/llvm_dispatch_fresh_transfer.gti",
    "fresh-hierarchy-copy-view": "tests/fixtures/llvm_dispatch_fresh_copy_view.gti",
    "inline-hierarchy-fields": "tests/fixtures/llvm_dispatch_inline_fields.gti",
    "hierarchy-reference-results": "examples/llvm/borrowed-hierarchy-owners.gti",
    "nested-hierarchy-transfer": "examples/llvm/nested-hierarchy-transfer.gti",
    "dispatch-cost-correctness": "tests/fixtures/llvm_dispatch_execution_cost.gti",
    "shipped-example": "examples/llvm/inheritance-and-dispatch.gti",
    "polymorphic-owners": "tests/fixtures/llvm_dispatch_polymorphic_owners.gti",
    "virtual-signatures": "tests/fixtures/llvm_dispatch_virtual_signatures.gti",
    "fresh-publication": "tests/fixtures/llvm_dispatch_fresh_publication.gti",
    "inline-slots": "tests/fixtures/llvm_dispatch_inline_slots.gti",
    "owned-transfers": "tests/fixtures/llvm_dispatch_owned_transfers.gti",
    "consuming-receivers": "tests/fixtures/llvm_dispatch_consuming_receivers.gti",
    "closure-models": "tests/fixtures/llvm_dispatch_closure_models.gti",
    "abstract-phases": "tests/fixtures/llvm_dispatch_abstract_phases.gti",
    "temporary-receivers": "tests/fixtures/llvm_dispatch_temporary_receivers.gti",
    "replacements": "tests/fixtures/llvm_dispatch_replacements.gti",
    "hierarchy-arrays": "tests/fixtures/llvm_dispatch_hierarchy_arrays.gti",
    # A field default's closure is lowered by each body running that default;
    # its instances agree on captures, so the model relocates into its slot.
    "field-default-closure-former-reject": "tests/fixtures/constructor_callback_field.gti",
    "program-storage": "tests/fixtures/llvm_dispatch_program_storage.gti",
    "shared-polymorphic-owners": "tests/fixtures/llvm_dispatch_shared_owners.gti",
    "lifecycle-phases": "tests/fixtures/llvm_dispatch_lifecycle_phases.gti",
}

TRANSFER_ENDPOINTS = (
    *ENDPOINTS[:2],
    *(Endpoint("llvm", level) for level in ("O0", "O1", "O2", "O3")),
)

CPP_LIMITATIONS = {
    "hierarchy-reference-results": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_entry'",
    "owner-reference-failure": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_std::__gti_inst_8743748746c3906f3cb32abc5c52a3d391e4507268de72665a366361f520203d_make_unique'",
    "owner-failure-before-reference": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_std::__gti_inst_1542b921ce78875e430a1cd950d1c99cce26c90908184496243024d42a33bfd9_make_unique'",
    "shared-owner-reference": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_std::__gti_inst_6c747ef376f3f4cfb185d24c782b604dd9912f48182009d7ad4f7242536225ae_make_shared'",
    "inline-hierarchy-fields": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_entry'",
    "inline-field-virtual-failure": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_entry'",
    "inline-field-constructor-rollback": "verified MIR body is outside the complete C++ text vocabulary for '::__gti_program::__gti_entry'",
    "borrowed-mut-reference-chain": "outside the complete C++ text vocabulary",
    "fresh-hierarchy-copy-view": "generated copy/move construction lacks the complete generic destination and cleanup schedule",
}

# Known C++-backend divergences, not oracle evidence (ADR 031: C++ is not an
# oracle for a shape it does not represent correctly). The C++ backend runs
# GTI cleanup while the C++ object is still whole, so a base destructor or a
# constructor rollback dispatches to the most-derived override, into destroyed
# or unconstructed state. The owner selected the destructor's own class
# (2026-10-27); LLVM must produce each case's independent expected value. The
# divergent C++ result is pinned so that a C++ fix is reviewed here.
CPP_DIVERGENCES = {
    "lifecycle-phases": (2, ""),
    "lifecycle-phase-rollback": (70, "GTI-R0014"),
    "lifecycle-phase-wrong-cleanup": (70, "GTI-R0014"),
    "cleanup-helper-cycle-base-phase-former-reject": (1, ""),
}

SUCCESS_SOURCES = {
    # Verbatim former llvm_scalar_execution_test.py "polymorphic-owner"
    # rejection. Its removal there follows actual native O0/O3 success here.
    "fresh-local-virtual-call": "class Owner { public: virtual int read() { return 0; } }; int main() { Owner value{}; return value.read(); }",
    "nonobserving-cleanup-helper-cycle": """mut int observed = 0;
class Numeric {
  int value = 7;
public:
  Numeric() {}
  virtual int read() { return this.value; }
  int payload() { return this.value; }
  static int first(Numeric& value, int remaining) {
    if (remaining == 0) { return value.payload(); }
    return Numeric::second(value, remaining - 1);
  }
  static int second(Numeric& value, int remaining) {
    return Numeric::first(value, remaining);
  }
  ~Numeric() { observed = Numeric::first(this, 2); }
};
int main() { { Numeric value{}; } return observed == 7 ? 0 : 1; }
""",
    "scalar-state-virtual-control": """class Plain {
public:
  mut int value;
  Plain() : value(1) {}
  virtual int read() { return this.value; }
};
int main() { Plain first{}; return first.read() - 1; }
""",
}

ALIAS_ANCHOR = "mut Leaf value{7};"

ROLLBACK_MAIN = """// DISPATCH_LIFECYCLE_MAIN
int main() {
  expected_cleanup = 1221;
  Guard guard{};
  Leaf value{0};
  return 0;
}
"""

PLAIN = """class Plain {
public:
  mut int value;
  Plain() : value(1) {}
  virtual int read() { return this.value; }
};
"""

SUCCESS_SOURCES_CYCLE = """mut int observed = 0;
class Numeric {
  int value = 7;
public:
  Numeric() {}
  virtual int read() { return this.value; }
  int payload() { return this.value; }
  static int first(Numeric& value, int remaining) {
    if (remaining == 0) { return value.payload(); }
    return Numeric::second(value, remaining - 1);
  }
  static int second(Numeric& value, int remaining) {
    return Numeric::first(value, remaining);
  }
  ~Numeric() { observed = Numeric::first(this, 2); }
};
int main() { { Numeric value{}; } return observed == 7 ? 0 : 1; }
"""

TRANSFER_SUCCESS_SOURCES = {
    # Verbatim former rejections. Their normal exit1 is successful execution of
    # these source contracts, not a failed runtime check or a rewritten oracle.
    "generated-copy-former-reject": (PLAIN + "int main() { Plain first{}; Plain second = first; return second.read(); }\n", 1),
    "generated-move-former-reject": (PLAIN + "int main() { mut Plain first{}; Plain second = std::move(first); return second.read(); }\n", 1),
    "fresh-copy-former-cli-control": ("class Owner { public: virtual int read() { return 0; } }; int main() { Owner value{}; Owner copied = value; return copied.read(); }\n", 0),
    "nested-object-former-reject": (PLAIN + "struct Wrapper { Plain value = Plain(); };\nint main() { Wrapper value{}; return value.value.read(); }\n", 1),
    "fresh-value-argument-former-reject": (PLAIN + "void take(Plain value) {}\nint main() { take(Plain()); return 0; }\n", 0),
    "fresh-return-former-reject": (PLAIN + "Plain make() { return Plain(); }\nint main() { Plain value = make(); return value.read(); }\n", 1),
    "local-return-former-reject": (PLAIN + "Plain make() { Plain local{}; return local; }\nint main() { Plain value = make(); return value.read(); }\n", 1),
    # An abstract phase that nothing observes installs no pure state.
    "pure-active-phase-former-reject": ("""class Base {
public:
  Base() {}
  virtual int read() = 0;
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int read() override { return 1; }
};
int main() { Derived value{}; return value.read(); }
""", 1),
    # A destructor of a class nothing derives from runs under the object's
    # own complete state, so its dispatch observes nothing ambiguous.
    "destructor-virtual-observation-former-reject": ("""class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  ~Observed() { int observed = this.read(); }
};
int main() { Observed value{}; return value.read() - 1; }
""", 0),
    "destructor-helper-observation-former-reject": ("""class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  int observe() { return this.read(); }
  ~Observed() { int observed = this.observe(); }
};
int main() { Observed value{}; return value.read() - 1; }
""", 0),
    "read-borrow-cleanup-observation-former-reject": ("""class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  static int through(Observed& value) { return value.read(); }
  ~Observed() { int observed = Observed::through(this); }
};
int main() { Observed value{}; return 0; }
""", 0),
    "mut-borrow-cleanup-observation-former-reject": ("""class Observed {
public:
  mut int value = 0;
  Observed() {}
  virtual void touch() mut { this.value += 1; }
  static void through(mut Observed& value) { value.touch(); }
  ~Observed() { Observed::through(this); }
};
int main() { Observed value{}; return 0; }
""", 0),
    "owned-destructor-observation-former-reject": ("""#include <std/memory>
class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  ~Observed() { int observed = this.read(); }
};
int main() { auto owner = std::make_unique<Observed>(); return owner->read() - 1; }
""", 0),
    "cleanup-helper-cycle-observation-former-reject": (SUCCESS_SOURCES_CYCLE.replace(
        "return value.payload();", "return value.read();"), 0),
    # A constructor publishing an interface reference field stores the view.
    "stored-interface-reference": ("""interface Widget { int width(); };
class Button : public Widget {
  int size;
public:
  Button(int size) : size(size) {}
  int width() override { return this.size; }
};
class Measurer {
  Widget& target;
public:
  Measurer(Widget& target) : target(target) {}
  int twice() { return this.target.width() * 2; }
};
int main() {
  Button button = Button(7);
  Measurer measurer = Measurer(button);
  return measurer.twice() == 14 ? 0 : 1;
}
""", 0),
    # Copy assignment of a complete inert polymorphic object.
    "assignment-former-reject": (PLAIN + "int main() { mut Plain first{}; Plain second{}; first = second; return first.read(); }\n", 1),
    "array-former-reject": (PLAIN + "int main() { Plain values[1] = {Plain()}; return values[0].read(); }\n", 1),
    "global-former-reject": (PLAIN + "Plain value{};\nint main() { return value.read(); }\n", 1),
    # A moved owned local with active cleanup leaves through the return edge.
    "returned-owned-move": ("""mut int dropped = 0;
class Owned {
  int id;
public:
  Owned(int id) : id(id) {}
  ~Owned() { dropped = dropped * 10 + this.id; }
  virtual int read() { return this.id; }
};
Owned relay(Owned value) {
  mut Owned local = std::move(value);
  return std::move(local);
}
int main() {
  {
    Owned kept = relay(Owned(4));
    if (kept.read() != 4 || dropped != 0) { return 1; }
  }
  return dropped == 4 ? 0 : 2;
}
""", 0),
    "heap-polymorphic-owner-former-reject": ("""#include <std/memory>
mut int dropped = 0;
class HeapValue {
  int value;
public:
  HeapValue(int value) : value(value) {}
  ~HeapValue() { dropped += this.value; }
  virtual int read() { return this.value; }
};
int main() {
  { auto owner = std::make_unique<HeapValue>(7);
    if (owner->read() != 7) { return 1; }
  }
  return dropped == 7 ? 0 : 2;
}
""", 0),
}

REJECTIONS = {
    "reject-move-construction": PLAIN + "int main() { mut Plain first{}; Plain second = Plain(std::move(first)); return second.read(); }\n",
    "reject-base-assignment": PLAIN + """class Derived : public Plain {};
void replace(mut Plain& destination, Plain& source) { destination = source; }
int main() { mut Derived first{}; Derived second{}; replace(first, second); return first.read(); }
""",
    # Arrays publish fresh elements in place; copying or passing a whole
    # hierarchy array still transports its objects.
    "reject-array-copy": PLAIN + "int main() { Plain values[1] = {Plain()}; Plain copied[1] = values; return copied[0].read(); }\n",
    "reject-array-argument": PLAIN + "int sum(Plain values[1]) { return values[0].read(); }\nint main() { Plain values[1] = {Plain()}; return sum(values); }\n",
    "reject-thread-local": PLAIN + "thread_local Plain value{};\nint main() { return value.read(); }\n",
    # A pure member selected while a constructor that never hands the object to
    # other code runs is unobservable; this phase is not, so it stays gated.
    "reject-observed-pure-phase": """class Base {
public:
  Base() { int observed = this.describe(); }
  virtual int read() = 0;
  virtual int describe() { return 2; }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int read() override { return 1; }
};
int main() { Derived value{}; return value.read(); }
""",
}

# Former destructor-observation rejections. A base destructor runs under its
# own class (owner decision, 2026-10-27), directly, through a helper or a
# terminating helper cycle, and during constructor rollback.
LIFECYCLE_FORMER_REJECTS = {
    "cleanup-helper-cycle-base-phase-former-reject": (SUCCESS_SOURCES[
        "nonobserving-cleanup-helper-cycle"].replace(
            "return value.payload();", "return value.read();").replace(
            "int main() { { Numeric value{}; } return observed == 7 ? 0 : 1; }",
            "class Wider : public Numeric {\npublic:\n  Wider() : Numeric() {}\n"
            "  int read() override { return 8; }\n};\n"
            "int main() { { Wider value{}; } return observed == 7 ? 0 : 1; }"), 0),
    "base-destructor-observation-former-reject": ("""class Base {
public:
  Base() {}
  virtual int read() { return 1; }
  ~Base() { int observed = this.read(); }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int read() override { return 2; }
};
int main() { Derived value{}; return value.read() - 2; }
""", 0),
    "base-helper-observation-former-reject": ("""class Base {
public:
  Base() {}
  virtual int read() { return 1; }
  static int through(Base& value) { return value.read(); }
  ~Base() { int observed = Base::through(this); }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int read() override { return 2; }
};
int main() { Derived value{}; return 0; }
""", 0),
}
LIFECYCLE_ROLLBACK_FORMER_REJECT = """class Base {
public:
  Base() {}
  virtual int read() { return 1; }
  ~Base() { int observed = this.read(); }
};
class Derived : public Base {
  int quotient;
public:
  Derived(int divisor) : Base(), quotient(12 / divisor) {}
  int read() override { return 2; }
};
int main() { Derived value{0}; return value.read(); }
"""

SOURCE_REJECTIONS = {
    "reject-source-return-local-view": PLAIN + "Plain& invalid() { Plain local{}; return local; }\nint main() { return invalid().read(); }\n",
    "reject-source-mutable-return-readonly": PLAIN + "mut Plain& invalid(Plain& value) { return value; }\nint main() { Plain value{}; return invalid(value).read(); }\n",
    "reject-source-deleted-copy": """class NoCopy {
public:
  NoCopy() {}
  NoCopy(NoCopy&) = delete;
  virtual int read() { return 0; }
};
int main() { NoCopy first{}; NoCopy second{first}; return second.read(); }
""",
    "reject-source-use-after-move": PLAIN + "int main() { mut Plain first{}; Plain second = std::move(first); return first.read(); }\n",
    "reject-source-mutable-view-from-readonly": PLAIN + """int main() {
  Plain value{};
  mut Plain& invalid = value;
  return invalid.read();
}
""",
    "reject-source-virtual-signature-mismatch": """interface Reader { int read(); };
class Wrong : public Reader { public: bool read() override { return true; } };
int main() { Wrong value{}; return 0; }
""",
    "reject-source-unresolved-override": """interface Reader { int read(); };
class Missing : public Reader { public: int read() override; };
int main() { Missing value{}; return value.read(); }
""",
}

SOURCE_REJECTION_CODES = {
    "reject-source-deleted-copy": "GTI-S2020",
    "reject-source-use-after-move": "GTI-S2018",
}


def invoke(command: list[str], root: Path, env: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=root, env=env, text=True,
                          capture_output=True, check=False, timeout=240)


def assert_source_mir(gti: Path, root: Path, work: Path, source: Path,
                      env: dict[str, str]) -> None:
    """A pending backend shape must not be mistaken for a source rejection."""
    destination = work / "source.mir"
    result = invoke([str(gti), str(source), "--emit-mir", "-O0", "-o", str(destination)], root, env)
    if result.returncode != 0 or not destination.is_file():
        raise AssertionError(f"source/MIR rejection in {source}: {result}")
    with destination.open(encoding="utf-8") as stream:
        header = stream.readline().strip()
    if not header.startswith("mir-v") or "valid=1" not in header:
        raise AssertionError(f"unverified source MIR for {source}: {header}")


def expected_failure(result, source: Path, line: int) -> bool:
    return (result.returncode == 70 and not result.stdout
            and f"{source.name}:{line}: runtime error[GTI-R0002]: division_by_zero in integer_division" in result.stderr
            and "GTI-R0014" not in result.stderr)


def execute_case(gti: Path, root: Path, work: Path, name: str,
                 source: Path, env: dict[str, str], *,
                 failure_needle: str | None = None,
                 expected_exit: int = 0,
                 cpp_limitation: str | None = None,
                 endpoints: tuple[Endpoint, ...] = ENDPOINTS,
                 cpp_divergence: tuple[int, str] | None = None) -> None:
    observables = []
    if cpp_limitation:
        mir_case = work / f"{name}-source-mir"
        mir_case.mkdir()
        assert_source_mir(gti, root, mir_case, source, env)
    failure_line = None
    if failure_needle is not None:
        matches = [index for index, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1)
                   if failure_needle in line]
        if len(matches) != 1:
            raise AssertionError(f"{name}: failure source needle is not unique")
        failure_line = matches[0]
    for endpoint in endpoints:
        case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard or 'native'}"
        result = execute(gti, root, case, source, endpoint, env)
        if cpp_limitation and endpoint.backend == "cpp":
            match = re.search(r"backend diagnostics saved at ([^\n]+)", result.stderr)
            detail = Path(match[1]).read_text(encoding="utf-8") if match else ""
            if (result.compile_returncode != 65 or "GTI-B0001" not in result.stderr
                    or cpp_limitation not in detail):
                raise AssertionError(f"{name}: C++ comparator status changed; review coverage: {result}")
            print(f"{name} {endpoint.standard}: C++ comparator REJECTED (not execution evidence)", flush=True)
            print(f"command: {result.command}\n{result.stderr}\n{detail}", flush=True)
            continue
        if cpp_divergence and endpoint.backend == "cpp":
            if (result.error or result.returncode != cpp_divergence[0]
                    or cpp_divergence[1] not in result.stderr):
                raise AssertionError(f"{name}: pinned C++ divergence changed; review coverage: {result}")
            print(f"{name} {endpoint.standard}: C++ comparator DIVERGES as pinned (known C++ backend defect, "
                  f"not execution evidence)", flush=True)
            continue
        if result.error:
            raise AssertionError(f"{name} {endpoint}: {result}")
        if failure_needle is None:
            if result.observable != (expected_exit, "", ""):
                raise AssertionError(f"{name} {endpoint}: {result}")
        elif not expected_failure(result, source, failure_line):
            raise AssertionError(f"{name} {endpoint}: {result}")
        observables.append(result.observable)
    if len(set(observables)) != 1:
        raise AssertionError(f"{name}: backend observable/source-attribution drift: {observables}")
    levels = "/".join(endpoint.optimization for endpoint in endpoints if endpoint.backend == "llvm")
    if cpp_limitation:
        print(f"{name}: verified source/MIR and native {levels} passed; C++ failures retained", flush=True)
    elif cpp_divergence:
        print(f"{name}: native {levels} passed its independent expectation; C++ divergence pinned", flush=True)
    else:
        outcome = f"normal exit {expected_exit}" if failure_needle is None else "source-attributed failure exit 70"
        print(f"{name}: C++20/23 O3 and native {levels} passed ({outcome})", flush=True)


def reject_case(gti: Path, root: Path, work: Path, name: str,
                text: str, env: dict[str, str]) -> None:
    case = work / name
    case.mkdir()
    source = case / f"{name}.gti"
    source.write_text(text, encoding="utf-8")
    assert_source_mir(gti, root, case, source, env)
    for mode in ("O0", "O3"):
        for existing in (False, True):
            output = case / f"program-{mode}-{'existing' if existing else 'absent'}"
            sentinel = b"existing dispatch artifact\n"
            if existing:
                output.write_bytes(sentinel)
            result = invoke([str(gti), str(source), "--backend", "llvm", f"-{mode}", "-o", str(output)], root, env)
            preserved = output.is_file() and output.read_bytes() == sentinel if existing else not output.exists()
            if result.returncode != 65 or "GTI-B0003" not in result.stderr or not preserved:
                raise AssertionError(f"{name} {mode} existing={existing}: {result}")
            expected = ("DestructorDispatchObservation" if "observation" in name
                        else "PureDispatchState" if name == "reject-observed-pure-phase"
                        else None if name in {"reject-global", "reject-thread-local"}
                        else "PolymorphicTransfer")
            if expected and f"Shape.{expected}" not in result.stderr:
                raise AssertionError(f"{name}: rejection did not exercise {expected}: {result}")
            if list(case.rglob("*.o")) or list(case.rglob("*.obj")):
                raise AssertionError(f"{name}: rejected native source published an object")
    print(f"{name}: verified source, native O0/O3 rejection and artifact preservation passed", flush=True)


# This opt-in path deliberately shares the source/execution harness, not the
# default correctness gate. Preparation never authorizes runtime sampling.
MEASURE_SCHEMA = 1
MEASURE_ENDPOINTS = (Endpoint("llvm", "O3"), *ENDPOINTS[:2])
MEASURE_VARIANTS = ("direct", "dynamic", "startup", "control")
MEASURE_ENVIRONMENT = (
    "PATH", "GTI_CXX", "CXX", "CC", "GTI_RUNTIME_LIBRARY", "GTI_RUNTIME_INCLUDE",
    "GTI_STDLIB_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "CPATH",
    "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT",
    "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
    "DYLD_INSERT_LIBRARIES", "ASAN_OPTIONS", "UBSAN_OPTIONS", "NO_COLOR",
)


def measure_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def measure_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def measure_digest(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def measure_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def measure_endpoint(endpoint: Endpoint) -> str:
    return f"{endpoint.backend}-{endpoint.optimization}-{endpoint.standard or 'native'}"


def measure_options(arguments) -> dict:
    return {"count_per_receiver": arguments.measure_count,
            "sample_rounds": arguments.measure_samples,
            "target_batch_seconds": arguments.measure_target_seconds,
            "max_batch_repetitions": arguments.measure_max_batch}


def measure_sources(count: int) -> tuple[dict[str, str], dict]:
    def expected(amount: int) -> int:
        cycles, remainder = divmod(count, 256)
        return cycles * (32640 + 256 * amount) + remainder * (remainder - 1) // 2 + remainder * amount

    # Each monotone subtotal (including the perturbed target) stays in int32.
    if count < 256 or max(expected(amount) for amount in (3, 4, 7)) > 2147483647:
        raise ValueError("measurement count must be >=256 with every subtotal in signed int32")
    prefix = """// Generated matched successful-execution source contracts; no noinline/FFI escape.
interface Step { int apply(int value); };
class Offset : public Step {
  int amount;
public:
  Offset(int amount) : amount(amount) {}
  int exact(int value) { return value + this.amount; }
  int apply(int value) override { return this.exact(value); }
};
class Seven : public Step {
public:
  int exact(int value) { return value + 7; }
  int apply(int value) override { return this.exact(value); }
};
int direct_offset_sum(Offset& step, int count) {
  mut int total = 0;
  for (mut int index = 0; index < count; index++) { total += step.exact(index & 255); }
  return total;
}
int direct_seven_sum(Seven& step, int count) {
  mut int total = 0;
  for (mut int index = 0; index < count; index++) { total += step.exact(index & 255); }
  return total;
}
int dynamic_sum(Step& step, int count) {
  mut int total = 0;
  for (mut int index = 0; index < count; index++) { total += step.apply(index & 255); }
  return total;
}
"""
    sources = {}
    for variant in MEASURE_VARIANTS:
        if variant == "startup":
            body = "return offset.exact(0) == 3 && seven.exact(0) == 7 ? 0 : 1;"
        else:
            first = "direct_offset_sum" if variant == "direct" else "dynamic_sum"
            second = "direct_seven_sum" if variant == "direct" else "dynamic_sum"
            body = (f"int first = {first}(offset, {count});\n"
                    f"  int second = {second}(seven, {count});\n"
                    f"  return first == {expected(3)} && second == {expected(7)} ? 0 : 1;")
        selected = prefix
        if variant == "control":
            # Change the actual virtual result, NOT the expected checksum. The
            # unchanged oracle must detect the extra one on every Offset call.
            selected = selected.replace("return this.exact(value);", "return this.exact(value) + 1;", 1)
        sources[variant] = selected + "int main() {\n  Offset offset{3};\n  Seven seven{};\n  " + body + "\n}\n"
    return sources, {"count_per_receiver": count, "receiver_amounts": [3, 7],
                     "expected_subtotals": [expected(3), expected(7)],
                     "perturbed_first_subtotal": expected(4),
                     "logical_virtual_calls_per_dynamic_execution": 2 * count,
                     "runtime_arguments": [], "success_exit": 0, "control_exit": 1}


def measure_capture(command: list[str], root: Path, env: dict[str, str], prefix: Path,
                    *, resources: bool = False, timeout: int = 240) -> dict:
    actual = list(command)
    stats = prefix.with_suffix(".resources.txt")
    if resources:
        if sys.platform == "darwin":
            actual = ["/usr/bin/time", "-l", "-o", str(stats), *command]
        elif sys.platform.startswith("linux"):
            actual = ["/usr/bin/time", "-v", "-o", str(stats), *command]
        else:
            raise RuntimeError("measurement resource accounting requires macOS or Linux /usr/bin/time")
    command_log = prefix.with_suffix(".command.json")
    measure_json(command_log, {"command": command, "invocation": actual, "phase": "started"})
    started = time.perf_counter()
    result = subprocess.run(actual, cwd=root, env=env, text=True, capture_output=True,
                            check=False, timeout=timeout)
    elapsed = time.perf_counter() - started
    stdout, stderr = prefix.with_suffix(".stdout"), prefix.with_suffix(".stderr")
    stdout.write_text(result.stdout, encoding="utf-8")
    stderr.write_text(result.stderr, encoding="utf-8")
    record = {"command": command, "invocation": actual, "returncode": result.returncode,
              "stdout": str(stdout), "stderr": str(stderr)}
    if resources:
        raw = stats.read_text(encoding="utf-8")
        match = (re.search(r"(\d+)\s+maximum resident set size", raw) if sys.platform == "darwin"
                 else re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", raw))
        if not match:
            raise AssertionError(f"missing per-invocation maximum RSS: {stats}")
        record.update(wall_seconds=elapsed, peak_rss_bytes=int(match[1]) * (1 if sys.platform == "darwin" else 1024),
                      resources=str(stats), rss_scope="platform time per-invocation maximum; not simultaneous process-tree RSS")
    measure_json(command_log, record)
    return record


def measure_tool(path: str, root: Path, env: dict[str, str], prefix: Path,
                 version_arguments: tuple[str, ...] = ("--version",)) -> dict:
    # Keep the invocation spelling: resolving clang++ to clang changes its link-driver mode.
    executable = shutil.which(path)
    if executable is None:
        raise RuntimeError(f"measurement tool not found: {path}")
    record = measure_capture([executable, *version_arguments], root, env, prefix)
    return {"path": executable, "sha256": measure_hash(Path(executable)), "version": record}


def measure_host(root: Path, env: dict[str, str], work: Path) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    host = {"system": platform.system(), "release": platform.release(), "machine": platform.machine(),
            "platform": platform.platform(), "python": sys.version}
    if sys.platform == "darwin":
        cpu = measure_capture(["/usr/sbin/sysctl", "-a"], root, env, work / "host-cpu")
        host["cpu"] = "\n".join(line for line in Path(cpu["stdout"]).read_text(encoding="utf-8").splitlines()
                                  if line.startswith(("hw.model:", "hw.ncpu:", "hw.physicalcpu:", "hw.memsize:",
                                                      "hw.optional.", "machdep.cpu.brand_string:", "machdep.cpu.features:",
                                                      "machdep.cpu.leaf7_features:")))
        host["cpu_probe"] = cpu
        host["power"] = [measure_capture(["/usr/bin/pmset", "-g", mode], root, env, work / f"power-{mode}")
                         for mode in ("custom", "batt")]
    elif sys.platform.startswith("linux"):
        text = Path("/proc/cpuinfo").read_text(encoding="utf-8").split("\n\n", 1)[0]
        host["cpu"] = "\n".join(line for line in text.splitlines()
                                  if line.split(":", 1)[0].strip() in {"vendor_id", "cpu family", "model", "model name", "stepping", "flags"})
        host["logical_cpus"] = os.cpu_count()
        host["power"] = {str(path): path.read_text(encoding="utf-8").strip()
                         for path in Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor")}
    else:
        raise RuntimeError("measurement supports only the coordinated macOS arm64/Linux x86_64 host ABIs")
    host["fingerprint_sha256"] = measure_digest({key: value for key, value in host.items()
                                                if key not in {"python", "power", "cpu_probe"}})
    return host


def measure_inspect(executable: Path, root: Path, env: dict[str, str], work: Path, tools: dict) -> dict:
    disassembler = tools["disassembler"]["path"]
    command = ([disassembler, "-tvV", str(executable)] if Path(disassembler).name == "otool"
               else [disassembler, "-d", "--no-show-raw-insn", str(executable)])
    disassembly = measure_capture(command, root, env, work / "disassembly")
    if disassembly["returncode"] != 0:
        raise AssertionError(f"disassembly failed: {disassembly}")
    file_record = measure_capture([tools["file"]["path"], str(executable)], root, env, work / "binary-format")
    size_command = [tools["size"]["path"], "-m" if sys.platform == "darwin" else "-A", str(executable)]
    sizes = measure_capture(size_command, root, env, work / "sections")
    sections = []
    for line in Path(sizes["stdout"]).read_text(encoding="utf-8").splitlines():
        match = (re.match(r"\s*Section\s+(\S+):\s*(\d+)", line) if sys.platform == "darwin"
                 else re.match(r"\s*(\.\S+)\s+(\d+)\s+", line))
        if match:
            sections.append({"name": match[1], "bytes": int(match[2])})
    symbols, tables, table_bytes = None, None, None
    usable_symbol_sizes = False
    table_size_reason = "no size-bearing symbol tool available"
    if tools.get("nm"):
        symbols = measure_capture([tools["nm"]["path"], "-S", "--defined-only", str(executable)],
                                  root, env, work / "symbols")
        table_size_reason = f"symbol inspection failed (exit {symbols['returncode']}); no table-size evidence"
        if symbols["returncode"] == 0:
            tables = []
            for line in Path(symbols["stdout"]).read_text(encoding="utf-8").splitlines():
                match = re.match(r"\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S\s+(\S+)", line)
                reported_size = int(match[1], 16) if match else None
                usable_symbol_sizes |= reported_size is not None and reported_size > 0
                fields = line.split()
                name = fields[-1] if fields else ""
                if "__gti_dispatch_table_" in name or name.lstrip("_").startswith("ZTV"):
                    tables.append({"symbol": name, "reported_bytes": reported_size,
                                   "bytes": reported_size if reported_size and reported_size > 0 else None})
            if not usable_symbol_sizes:
                table_size_reason = "symbol tool supplied no usable nonzero sizes (all-zero, empty or unavailable metadata); table bytes unknown"
            elif any(row["bytes"] is None for row in tables):
                table_size_reason = "recognized table symbols lack usable nonzero sizes; partial totals are not size evidence"
            else:
                table_bytes = sum(row["bytes"] for row in tables)
                table_size_reason = ("sum of all recognized nonzero table sizes" if tables
                                     else "no recognized table symbols; nonzero sizes elsewhere establish usable symbol-size metadata")
    return {"disassembly": disassembly, "binary_format": file_record, "section_tool": sizes,
            "sections": sections, "executable_bytes": executable.stat().st_size,
            "text_bytes": sum(row["bytes"] for row in sections if row["name"] in {".text", "__text"}) if sections else None,
            "unwind_bytes": sum(row["bytes"] for row in sections if row["name"] in {
                ".eh_frame", ".eh_frame_hdr", ".gcc_except_table", "__eh_frame", "__unwind_info", "__gcc_except_tab"}) if sections else None,
            "table_symbols": tables, "dispatch_table_bytes": table_bytes,
            "dispatch_table_bytes_reason": table_size_reason, "symbol_size_metadata_usable": usable_symbol_sizes,
            "symbol_tool": symbols,
            "size_scope": "named text/unwind sections; all size-bearing GTI dispatch-table/C++ vtable symbols, including linked runtime; null means unavailable, not zero"}


def measure_expect(record: dict, expected: int) -> None:
    if (record["returncode"] != expected or Path(record["stdout"]).read_text(encoding="utf-8")
            or Path(record["stderr"]).read_text(encoding="utf-8")):
        raise AssertionError(f"measurement correctness/control failure: {record}")


def measure_prepare(gti: Path, root: Path, work: Path, env: dict[str, str], arguments) -> None:
    if work.exists() and any(work.iterdir()):
        raise RuntimeError("preparation requires a new/empty evidence directory; existing artifacts are never overwritten")
    work.mkdir(parents=True, exist_ok=True)
    metadata = work / "metadata"
    metadata.mkdir()
    sources, inputs = measure_sources(arguments.measure_count)
    measure_json(work / "inputs.json", inputs)
    toolchain = {"gti": measure_tool(str(gti), root, env, metadata / "gti-version"),
                 "cxx": measure_tool(env.get("GTI_CXX") or env.get("CXX") or "c++", root, env, metadata / "cxx-version")}
    tools = {}
    disassembler = shutil.which("llvm-objdump") or shutil.which("otool" if sys.platform == "darwin" else "objdump")
    if not disassembler:
        raise RuntimeError("preparation requires a native disassembler; no source-only proof fallback")
    for name, selected in (("disassembler", disassembler), ("file", "file"), ("size", "size")):
        tools[name] = measure_tool(selected, root, env, metadata / f"{name}-version")
    tools["time"] = {"path": "/usr/bin/time", "sha256": measure_hash(Path("/usr/bin/time")),
                     "version": "platform resource utility; identity bound by binary hash and OS"}
    nm = shutil.which("llvm-nm") or (shutil.which("nm") if sys.platform.startswith("linux") else None)
    if nm:
        tools["nm"] = measure_tool(nm, root, env, metadata / "nm-version")
    target = measure_capture([toolchain["cxx"]["path"], "-dumpmachine"], root, env, metadata / "native-target")
    toolchain["native_target"] = target
    toolchain["llvm_target_policy"] = "host target, generic CPU, empty feature string; no --target/CPU/features overrides"
    policy = root / "src/compiler/llvm/session.cpp"
    toolchain["llvm_target_policy_source"] = {"path": str(policy), "sha256": measure_hash(policy)}
    runtime = Path(env.get("GTI_RUNTIME_LIBRARY", str(gti.parent / "libgti_runtime.a")))
    if not runtime.is_file():
        raise RuntimeError(f"measurement requires a hash-bound runtime archive: {runtime}")
    toolchain["runtime_library"] = {"path": str(runtime.resolve()), "sha256": measure_hash(runtime)}
    host = measure_host(root, env, metadata)
    source_directory = work / "sources"
    source_directory.mkdir()
    programs = {}
    mir_targets = set()
    for variant, text in sources.items():
        source = source_directory / f"{variant}.gti"
        source.write_text(text, encoding="utf-8")
        mir_work = source_directory / f"{variant}-mir"
        mir_work.mkdir()
        mir = mir_work / "source.mir"
        emitted = measure_capture([str(gti), str(source), "--emit-mir", "-O0", "-o", str(mir)],
                                  root, env, mir_work / "emit")
        if emitted["returncode"] != 0 or not mir.is_file():
            raise AssertionError(f"measurement source/MIR rejection: {emitted}")
        with mir.open(encoding="utf-8") as stream:
            header = stream.readline().strip()
            target_line = stream.readline().strip()
        if not header.startswith("mir-v") or "valid=1" not in header:
            raise AssertionError(f"measurement source MIR is not verified: {mir}: {header}")
        if not target_line.startswith("target "):
            raise AssertionError(f"measurement source MIR lacks sealed target identity: {mir}")
        mir_targets.add(target_line)
    if len(mir_targets) != 1:
        raise AssertionError("measurement sources disagree on sealed target identity")
    toolchain["sealed_mir_target"] = next(iter(mir_targets))
    for endpoint in MEASURE_ENDPOINTS:
        label = measure_endpoint(endpoint)
        programs[label] = {}
        for variant in MEASURE_VARIANTS:
            case = work / label / variant
            case.mkdir(parents=True)
            source = source_directory / f"{variant}.gti"
            executable = case / "program"
            command = [str(gti), str(source), "--backend", endpoint.backend, "-O3", "--verbose",
                       "--cxx", toolchain["cxx"]["path"], "-o", str(executable)]
            if endpoint.standard:
                command += ["--std", endpoint.standard, "--keep-cpp"]
            build = measure_capture(command, root, env, case / "build", resources=True)
            if build["returncode"] != 0 or not executable.is_file():
                raise AssertionError(f"measurement build failed: {build}")
            correctness = measure_capture([str(executable)], root, env, case / "correctness")
            measure_expect(correctness, 1 if variant == "control" else 0)
            programs[label][variant] = {"source": str(source), "executable": str(executable),
                                        "build": build, "correctness": correctness,
                                        "inspection": measure_inspect(executable, root, env, case, tools)}
            for identity in (*toolchain.values(), *tools.values()):
                if isinstance(identity, dict) and "path" in identity and measure_hash(Path(identity["path"])) != identity["sha256"]:
                    raise RuntimeError(f"toolchain changed during preparation: {identity['path']}")
            for source_variant, text in sources.items():
                if (source_directory / f"{source_variant}.gti").read_text(encoding="utf-8") != text:
                    raise RuntimeError("measurement source changed during preparation")
    snapshot = measure_capture(["git", "rev-parse", "HEAD"], root, env, metadata / "source-head")
    dirty = measure_capture(["git", "diff", "--binary", "HEAD"], root, env, metadata / "source-diff")
    bound_files = {path.resolve() for path in work.rglob("*") if path.is_file()}
    bound_files.add(Path(__file__).resolve())
    for identity in (*toolchain.values(), *tools.values()):
        if isinstance(identity, dict) and "path" in identity:
            bound_files.add(Path(identity["path"]).absolute())
    request = {
        "schema": MEASURE_SCHEMA, "phase": "prepared-no-runtime-timing", "prepared_at_utc": measure_now(),
        "root": str(root), "options": measure_options(arguments), "inputs": inputs,
        "inputs_sha256": measure_hash(work / "inputs.json"), "toolchain": toolchain, "tools": tools,
        "host": host, "programs": programs,
        "harness_workspace_snapshot": {"head": snapshot, "diff": dirty,
                                       "scope": "current harness/workspace only; NOT the source/build identity of the compiler executable"},
        "compiler_build_provenance": {"status": "unavailable-pending-independent-review",
                                      "scope": "binary hash/version alone do not establish compiler source HEAD, build flags or host optimization; external build ledger required"},
        "environment": {name: env.get(name) for name in MEASURE_ENVIRONMENT},
        "environment_sha256": measure_digest(env),
        "artifacts": [{"path": str(path), "sha256": measure_hash(path)} for path in sorted(bound_files)],
        "contract": {
            "matched_source": "same scalar arithmetic, two receiver amounts, iteration counts and checksum/status checks; direct calls exact(), virtual apply() forwards exact()",
            "timing": "execution-only whole-process wall batches include launch, pipes/wait and harness checks, not loop-only/CPU time; startup is separate and never subtracted; RSS invocations are separate; no performance acceptance thresholds",
            "build_resources": "per-build wall includes GTI and native compiler/linker through platform time; maximum RSS is per-invocation platform accounting, not summed children or simultaneous process-tree peak",
            "environment": "full inherited environment is hash-bound, not sanitized; only selected toolchain/loader/sanitizer values are disclosed",
            "abi": "same-backend source-cost comparison, not identical physical ABI; LLVM checked/status propagation and C++ exception/adaptation/failure contracts differ",
            "allocation": "NOT MEASURED: no provider counter in this slice; source shape/RSS cannot establish zero allocations; scoped provider evidence remains an acceptance prerequisite",
            "proof_boundary": "the reviewer, not a regex or symbol-presence check, establishes executed normal-path control/data flow; an unused generic indirect body is insufficient",
            "fallback": "if LLVM O3 folds/devirtualizes the hosted normal workload, STOP: a separately approved same-planner test consumer is a prerequisite, not an unchecked fallback"},
        "review_requirements": {
            "schema": MEASURE_SCHEMA, "approval_scope": "artifact-control-flow-review",
            "approval_requirement": "separate external review must explicitly set approved=true; this request is NOT approval",
            "request_sha256": "SHA256 of this exact review-request.json", "reviewer": "independent human/coordinator identity",
            "reviewed_at_utc": "ISO8601 UTC", "artifact_proofs": "one entry for every endpoint/direct and endpoint/dynamic",
            "contract_acknowledgements": ["source_inputs_and_target_change_control", "same_backend_not_identical_failure_abi", "unavailable_allocations_not_zero"],
            "compiler_build_evidence": {"source_head": "exact compiler build-source commit, independent of harness_workspace_snapshot",
                                        "build_type": "Release, RelWithDebInfo or MinSizeRel",
                                        "binary_sha256": "must match toolchain.gti.sha256",
                                        "build_log": "{path: absolute retained external build/flags log or ledger, sha256: exact content hash}; reviewer establishes its binding to this source/build/binary"},
            "classifications": ["retained-dispatch", "matched-direct", "optimized-source-contract"],
            "references": "{line: one-based disassembly stdout line, text: that exact line}; mechanical validation checks only references, never approves semantics",
            "each_proof": ["classification", "normal_path_description", "normal_path (nonempty list of references)", "status_success (reference)"],
            "retained_dispatch_proof": ["loop_header", "backedge", "indirect_call", "result_publication", "result_read", "checksum_consumption", "indirect_result_flow_description", "logical_calls_per_execution"],
            "result_read": "identify actual load or register use and explain exact result transport; do not claim a memory load when optimization kept the value in registers",
            "native_requirement": "llvm-O3-native/dynamic MUST be retained-dispatch, reachable from hosted entry on the normal successful path, with consumed checksum and status-success control",
            "cpp_requirement": "C++20/23 are required source comparators; optimized-source-contract explicitly makes NO retained dispatch-cost claim",
            "host_plan": {"schema": MEASURE_SCHEMA, "approval_scope": "runtime-sampling",
                          "approval_requirement": "separate external plan with approved=true",
                          "required_fields": ["request_sha256", "review_sha256", "host_fingerprint_sha256", "approved_by",
                                              "reservation", "uncontended=true", "power_policy", "valid_from_utc", "valid_until_utc"]}}}
    measure_json(work / "review-request.json", request)
    print(f"Prepared correctness/control, build resources and disassembly: {work / 'review-request.json'}", flush=True)
    print("STOPPED before runtime timing. Independent artifact review and a separate approved host window are required.", flush=True)


def measure_verify_artifacts(request: dict) -> None:
    for artifact in request["artifacts"]:
        path = Path(artifact["path"])
        if not path.is_file() or measure_hash(path) != artifact["sha256"]:
            raise RuntimeError(f"stale measurement artifact; new preparation/review required: {path}")


def measure_reference(reference: dict, lines: list[str], label: str) -> None:
    line = reference.get("line") if isinstance(reference, dict) else None
    if type(line) is not int or line < 1 or line > len(lines) or reference.get("text") != lines[line - 1]:
        raise RuntimeError(f"invalid disassembly reference: {label}")


def measure_review(request: dict, request_hash: str, path: Path) -> dict:
    review = json.loads(path.read_text(encoding="utf-8"))
    if (review.get("schema") != MEASURE_SCHEMA or review.get("approved") is not True
            or review.get("approval_scope") != "artifact-control-flow-review"
            or review.get("request_sha256") != request_hash or not review.get("reviewer")
            or not review.get("reviewed_at_utc")):
        raise RuntimeError("missing/stale independent artifact review; no automatic approval or timing fallback")
    reviewed = measure_utc(review["reviewed_at_utc"])
    if not measure_utc(request["prepared_at_utc"]) <= reviewed <= datetime.now(timezone.utc):
        raise RuntimeError("artifact review must follow exact preparation and may not be future-dated")
    acknowledgements = review.get("contract_acknowledgements", {})
    if any(acknowledgements.get(key) is not True for key in (
            "source_inputs_and_target_change_control", "same_backend_not_identical_failure_abi", "unavailable_allocations_not_zero")):
        raise RuntimeError("artifact reviewer has not acknowledged the source/control, failure-ABI and allocation boundaries")
    compiler = review.get("compiler_build_evidence", {})
    build_log = compiler.get("build_log", {})
    if (compiler.get("binary_sha256") != request["toolchain"]["gti"]["sha256"]
            or compiler.get("build_type") not in {"Release", "RelWithDebInfo", "MinSizeRel"}
            or not isinstance(compiler.get("source_head"), str)
            or not re.fullmatch(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})", compiler["source_head"])
            or not isinstance(build_log, dict) or not isinstance(build_log.get("path"), str)
            or not Path(build_log["path"]).is_absolute() or not isinstance(build_log.get("sha256"), str)):
        raise RuntimeError("missing exact external compiler source/optimized-build/binary/log provenance; workspace HEAD is not compiler build evidence")
    measure_verify_artifacts({"artifacts": [build_log]})
    proofs = review.get("artifact_proofs", {})
    for label, variants in request["programs"].items():
        for variant in ("direct", "dynamic"):
            key = f"{label}/{variant}"
            proof = proofs.get(key, {})
            allowed = {"matched-direct", "optimized-source-contract"} if variant == "direct" else {"retained-dispatch", "optimized-source-contract"}
            if (proof.get("classification") not in allowed or not proof.get("normal_path_description")
                    or not isinstance(proof.get("normal_path"), list) or not proof["normal_path"]):
                raise RuntimeError(f"missing explicit hosted-normal-path artifact review: {key}")
            lines = Path(variants[variant]["inspection"]["disassembly"]["stdout"]).read_text(encoding="utf-8").splitlines()
            for index, reference in enumerate(proof["normal_path"]):
                measure_reference(reference, lines, f"{key}/normal_path/{index}")
            measure_reference(proof.get("status_success"), lines, f"{key}/status_success")
            if label == "llvm-O3-native" and variant == "dynamic" and proof["classification"] != "retained-dispatch":
                raise RuntimeError("LLVM hosted workload has no reviewed retained indirect execution; same-planner consumer prerequisite remains open")
            if proof["classification"] == "retained-dispatch":
                for role in ("loop_header", "backedge", "indirect_call", "result_publication", "result_read", "checksum_consumption"):
                    measure_reference(proof.get(role), lines, f"{key}/{role}")
                if (not proof.get("indirect_result_flow_description") or proof.get("logical_calls_per_execution")
                        != request["inputs"]["logical_virtual_calls_per_dynamic_execution"]):
                    raise RuntimeError(f"missing reviewed result/checksum/call-count proof: {key}")
    return review


def measure_utc(value: str) -> datetime:
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.tzinfo is None or result.utcoffset().total_seconds() != 0:
        raise RuntimeError("approval times must carry an explicit UTC timezone")
    return result


def measure_host_plan(path: Path, request: dict, request_hash: str, review_hash: str) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    if (plan.get("schema") != MEASURE_SCHEMA or plan.get("approved") is not True
            or plan.get("approval_scope") != "runtime-sampling" or plan.get("request_sha256") != request_hash
            or plan.get("review_sha256") != review_hash or plan.get("uncontended") is not True
            or plan.get("host_fingerprint_sha256") != request["host"]["fingerprint_sha256"]
            or not all(plan.get(key) for key in ("approved_by", "reservation", "power_policy", "valid_from_utc", "valid_until_utc"))):
        raise RuntimeError("missing/stale external uncontended-host approval; artifact review does not reserve a host")
    now = datetime.now(timezone.utc)
    if not measure_utc(plan["valid_from_utc"]) <= now <= measure_utc(plan["valid_until_utc"]):
        raise RuntimeError("runtime sampling is outside the explicitly approved host window")
    return plan


def measure_dispersion(values: list[float]) -> dict:
    middle = statistics.median(values)
    quartiles = statistics.quantiles(values, n=4, method="inclusive")
    return {"samples": len(values), "median_seconds": middle, "min_seconds": min(values),
            "max_seconds": max(values), "mad_seconds": statistics.median(abs(value - middle) for value in values),
            "iqr_seconds": quartiles[2] - quartiles[0]}


def measure_sample(executable: str, root: Path, env: dict[str, str], repetitions: int,
                   deadline: datetime) -> dict:
    started = time.perf_counter_ns()
    for _ in range(repetitions):
        remaining = (deadline - datetime.now(timezone.utc)).total_seconds()
        if remaining <= 0:
            raise RuntimeError("approved host window ended during runtime sampling")
        result = subprocess.run([executable], cwd=root, env=env, capture_output=True, check=False, timeout=min(240, remaining))
        if (result.returncode, result.stdout, result.stderr) != (0, b"", b""):
            raise AssertionError(f"timed artifact changed observable: {executable}: {result}")
    elapsed = (time.perf_counter_ns() - started) / 1e9
    return {"command": [executable], "repetitions": repetitions, "batch_wall_seconds": elapsed,
            "per_process_wall_seconds": elapsed / repetitions}


def measure_run(gti: Path, root: Path, work: Path, env: dict[str, str], arguments) -> None:
    request_path = work / "review-request.json"
    request_hash = measure_hash(request_path)
    request = json.loads(request_path.read_text(encoding="utf-8"))
    if (request.get("schema") != MEASURE_SCHEMA or request.get("phase") != "prepared-no-runtime-timing"
            or request.get("root") != str(root) or request.get("options") != measure_options(arguments)
            or request["toolchain"]["gti"]["path"] != str(gti)
            or request.get("environment") != {name: env.get(name) for name in MEASURE_ENVIRONMENT}
            or request.get("environment_sha256") != measure_digest(env)):
        raise RuntimeError("prepared source/options/compiler/environment identity changed; prepare and review again")
    review_path, host_path = arguments.measure_review.resolve(), arguments.measure_host_plan.resolve()
    review_hash, host_hash = measure_hash(review_path), measure_hash(host_path)
    measure_verify_artifacts(request)
    review = measure_review(request, request_hash, review_path)
    host_plan = measure_host_plan(host_path, request, request_hash, review_hash)
    deadline = measure_utc(host_plan["valid_until_utc"])
    run = work / ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    run.mkdir()
    host = measure_host(root, env, run)
    if host["fingerprint_sha256"] != request["host"]["fingerprint_sha256"]:
        raise RuntimeError("measurement host changed; preparation and host approval must be repeated")
    report = {"schema": MEASURE_SCHEMA, "started_at_utc": measure_now(), "status": "in-progress",
              "request_sha256": request_hash, "review": {"path": str(review_path), "sha256": review_hash},
              "host_plan": {"path": str(host_path), "sha256": host_hash, "reservation": host_plan["reservation"]},
              "host": host, "options": request["options"], "pilots": [], "warmups": [], "samples": [],
              "compiler_build_evidence": review["compiler_build_evidence"],
              "runtime_rss": {}, "summaries": {}, "allocation": request["contract"]["allocation"],
              "interpretation": request["contract"], "artifact_classifications": {
                  key: proof["classification"] for key, proof in review["artifact_proofs"].items()}}

    bound_artifacts = [*request["artifacts"], review["compiler_build_evidence"]["build_log"]]
    artifact_stats = {artifact["path"]: (Path(artifact["path"]).stat().st_size,
                                        Path(artifact["path"]).stat().st_mtime_ns,
                                        Path(artifact["path"]).stat().st_ctime_ns) for artifact in bound_artifacts}

    def validate(*, full: bool = False) -> None:
        if (measure_hash(request_path) != request_hash or measure_hash(review_path) != review_hash
                or measure_hash(host_path) != host_hash):
            raise RuntimeError("review/request/host approval changed during sampling; measurements invalid")
        # Do not heat the compiler/binary caches by rehashing the whole evidence
        # set between samples. Full hashes bracket sampling; metadata changes
        # invalidate the run immediately, even if a file is later restored.
        for path, initial in artifact_stats.items():
            current = Path(path).stat()
            if (current.st_size, current.st_mtime_ns, current.st_ctime_ns) != initial:
                raise RuntimeError(f"artifact changed during sampling: {path}")
        if full:
            measure_verify_artifacts({"artifacts": bound_artifacts})
        measure_host_plan(host_path, request, request_hash, review_hash)

    try:
        labels = list(request["programs"])
        batches = {}
        validate(full=True)
        for label in labels:
            variants = request["programs"][label]
            # Recheck exact prepared native observables, including the changed
            # virtual-target control, before calibration. No compilation here.
            for variant in MEASURE_VARIANTS:
                result = measure_capture([variants[variant]["executable"]], root, env, run / f"check-{label}-{variant}")
                measure_expect(result, 1 if variant == "control" else 0)
            pilots = {}
            for variant in ("direct", "dynamic", "startup"):
                pilot = measure_sample(variants[variant]["executable"], root, env, 1, deadline)
                report["pilots"].append({"endpoint": label, "variant": variant, **pilot})
                pilots[variant] = pilot["per_process_wall_seconds"]
            common = min(arguments.measure_max_batch,
                         max(1, math.ceil(arguments.measure_target_seconds / min(pilots["direct"], pilots["dynamic"]))))
            batches[label] = {"direct": common, "dynamic": common,
                              "startup": min(arguments.measure_max_batch, max(1, math.ceil(arguments.measure_target_seconds / pilots["startup"])))}
        report["batch_repetitions"] = batches
        report["calibration_scope"] = "fixed source inputs/builds; shared direct/dynamic process repetitions, capped, duration target only (not a performance threshold)"
        for warmup, rounds in ((True, 2), (False, arguments.measure_samples)):
            for round_index in range(rounds):
                validate()
                endpoint_order = labels if round_index % 2 == 0 else list(reversed(labels))
                for endpoint_index, label in enumerate(endpoint_order):
                    order = ["direct", "dynamic", "startup"]
                    if (round_index + endpoint_index) % 2:
                        order.reverse()
                    for variant in order:
                        sample = measure_sample(request["programs"][label][variant]["executable"], root, env, batches[label][variant], deadline)
                        report["warmups" if warmup else "samples"].append({"round": round_index,
                            "endpoint": label, "variant": variant, "order_in_round": len(report["warmups" if warmup else "samples"]), **sample})
                measure_json(run / "results.json", report)
        for label in labels:
            report["summaries"][label] = {}
            for variant in ("direct", "dynamic", "startup"):
                values = [sample["per_process_wall_seconds"] for sample in report["samples"]
                          if sample["endpoint"] == label and sample["variant"] == variant]
                report["summaries"][label][variant] = measure_dispersion(values)
                validate()
                rss = measure_capture([request["programs"][label][variant]["executable"]], root, env,
                                      run / f"runtime-rss-{label}-{variant}", resources=True,
                                      timeout=min(240, max(0.001, (deadline - datetime.now(timezone.utc)).total_seconds())))
                measure_expect(rss, 0)
                report["runtime_rss"][f"{label}/{variant}"] = rss
            summary = report["summaries"][label]
            summary["dynamic_to_direct_process_median_ratio"] = summary["dynamic"]["median_seconds"] / summary["direct"]["median_seconds"]
        validate(full=True)
        report["host_after_sampling"] = measure_host(root, env, run / "after-sampling")
        if report["host_after_sampling"]["fingerprint_sha256"] != host["fingerprint_sha256"]:
            raise RuntimeError("host CPU/OS identity changed during sampling")
        report.update(status="measured-not-performance-acceptance", finished_at_utc=measure_now())
    except BaseException as error:
        report.update(status="invalid-or-incomplete", error=str(error), finished_at_utc=measure_now())
        raise
    finally:
        measure_json(run / "results.json", report)
    print(f"Execution-only samples and dispersion retained: {run / 'results.json'}", flush=True)
    print("No speedup/parity/zero-allocation or acceptance claim is made; inspect contracts, review and missing provider evidence.", flush=True)


def measure_main(gti: Path, root: Path, env: dict[str, str], arguments) -> None:
    if arguments.cases:
        raise ValueError("--measure and --case are separate opt-in workflows")
    if arguments.measure_samples < 9 or arguments.measure_max_batch < 1 or arguments.measure_max_batch > 64:
        raise ValueError("measurement requires >=9 sample rounds and 1..64 maximum process repetitions")
    if not math.isfinite(arguments.measure_target_seconds) or not 0.1 <= arguments.measure_target_seconds <= 5.0:
        raise ValueError("calibration batch target must be finite and in 0.1..5 seconds")
    measure_sources(arguments.measure_count)
    if bool(arguments.measure_review) != bool(arguments.measure_host_plan):
        raise ValueError("runtime timing requires BOTH independent --measure-review and external --measure-host-plan")
    work = arguments.measure.resolve()
    if arguments.measure_review:
        measure_run(gti, root, work, env, arguments)
    else:
        measure_prepare(gti, root, work, env, arguments)



@dataclass(frozen=True)
class DispatchCase:
    source_text: str
    context: CaseContext
    kind: str
    expected_exit: int = 0
    failure_needle: str | None = None
    cpp_limitation: str | None = None
    endpoints: tuple[Endpoint, ...] = ENDPOINTS
    cpp_divergence: tuple[int, str] | None = None

    @property
    def is_control(self) -> bool:
        return self.kind in {"backend-rejection", "source-rejection"}

    def sidecar_record(self) -> dict:
        endpoints = (tuple(endpoint for endpoint in ENDPOINTS if endpoint.backend == "llvm")
                     if self.is_control else self.endpoints)
        expectation = {"kind": self.kind, "expected_exit": self.expected_exit,
                       "failure_needle": self.failure_needle,
                       "cpp_limitation": self.cpp_limitation}
        if self.cpp_divergence:
            expectation["cpp_divergence"] = {"exit": self.cpp_divergence[0],
                                             "stderr": self.cpp_divergence[1]}
        if self.kind == "backend-rejection":
            expectation.update(diagnostic="GTI-B0003", verified_source_mir=True,
                               artifact_preservation="absent-and-existing", no_object_publication=True)
        elif self.kind == "source-rejection":
            expectation.update(diagnostic="GTI-S", forbidden_diagnostic="GTI-B0003",
                               exact_code=SOURCE_REJECTION_CODES.get(self.context.identity),
                               artifact_preservation="existing")
        elif self.kind == "wrong-cleanup-trace":
            expectation.update(required_diagnostics=["GTI-R0014", "primary [GTI-R0002]"],
                               original_source_attribution=True, positive_failure_oracle_must_reject=True)
        elif self.kind == "defined-failure":
            expectation.update(required_diagnostic="GTI-R0002", forbidden_diagnostic="GTI-R0014",
                               original_source_attribution=True)
        if not self.is_control:
            expectation.update(stdout="", stderr="" if self.kind == "success" else "source-attributed-failure")
        return {
            "context": self.context.record(),
            "role": "expected-rejection-control" if self.is_control else "positive-admission",
            "observation": {
                "endpoints": [asdict(endpoint) for endpoint in endpoints],
                "setup": {"cwd": "repository-root", "environment": "copy-inherited",
                          "environment_overrides": {"NO_COLOR": "1"}, "argv": [],
                          "extra_native_inputs": [], "build_timeout": 240,
                          "run_timeout": None if self.is_control else 15},
                "expectation": expectation,
            },
        }


def dispatch_cases(root: Path) -> dict[str, DispatchCase]:
    """One byte/role authority for default execution and evidence listing."""
    cases = {}

    def add(name, source, kind="success", origin=None, expected_exit=0,
            failure_needle=None, cpp_limitation=None, endpoints=ENDPOINTS):
        if name in cases:
            raise ValueError(f"duplicate dispatch case: {name}")
        context = CaseContext(
            provider="llvm_dispatch_execution", identity=name,
            source=origin or f"inline/{name}.gti",
            source_sha256=hashlib.sha256(source.encode("utf-8")).hexdigest(),
            source_kind="repository" if origin else "inline",
            materialized_name=f"{name}.gti", execution_profile="single-threaded",
            loading="repository-source" if origin else "materialized-source-with-inherited-compiler-environment",
            observation="dispatch-rejection-control" if kind in {"backend-rejection", "source-rejection"}
                        else "dispatch-native-and-supported-comparator-execution",
            unresolved=("host-target-applicability", "effective-compiler-environment",
                        "implicit-toolchain-runtime", "compiler-build-source-identity"))
        cases[name] = DispatchCase(source, context, kind, expected_exit,
                                   failure_needle, cpp_limitation, endpoints,
                                   CPP_DIVERGENCES.get(name))

    for name, relative in SUCCESS_FILES.items():
        add(name, (root / relative).read_text(encoding="utf-8"), origin=relative,
            cpp_limitation=CPP_LIMITATIONS.get(name),
            endpoints=TRANSFER_ENDPOINTS if name.startswith("fresh-hierarchy-") or name in {"inline-hierarchy-fields", "hierarchy-reference-results", "nested-hierarchy-transfer", "polymorphic-owners", "virtual-signatures", "fresh-publication", "inline-slots", "owned-transfers", "consuming-receivers", "closure-models", "abstract-phases", "temporary-receivers", "replacements", "hierarchy-arrays", "field-default-closure-former-reject", "program-storage", "shared-polymorphic-owners", "lifecycle-phases"} else ENDPOINTS)
    for name, source in SUCCESS_SOURCES.items():
        add(name, source)
    for name, (source, expected_exit) in TRANSFER_SUCCESS_SOURCES.items():
        add(name, source, expected_exit=expected_exit, endpoints=TRANSFER_ENDPOINTS)
    source = (root / SUCCESS_FILES["static-dispatch"]).read_text(encoding="utf-8")
    nested = (root / SUCCESS_FILES["nested-hierarchy-transfer"]).read_text(encoding="utf-8")
    nested_failure = nested.replace("// NESTED_TRANSFER_MAIN", """
class Guard {
public:
  ~Guard() {
    if (events != 321) { int zero = 0; int checked_cleanup = 1 / zero; }
  }
};
""").replace("int main() {", "int main() { Guard guard{};").replace(
        "read(last.inner.second) != 2) { return 2; }",
        "read(last.inner.second) != 2) { return 2; }\n    int zero = 0; return 120 / zero;")
    add("nested-transfer-failure", nested_failure, "defined-failure", expected_exit=70,
        failure_needle="return 120 / zero;", endpoints=TRANSFER_ENDPOINTS)
    add("nested-transfer-wrong-cleanup", nested_failure.replace("events != 321", "events != 312"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="return 120 / zero;",
        endpoints=TRANSFER_ENDPOINTS)
    reference = (root / SUCCESS_FILES["hierarchy-reference-results"]).read_text(encoding="utf-8")
    reference_prefix, _ = reference.split("// REFERENCE_RESULTS_MAIN\n", 1)
    add("shared-owner-reference", reference_prefix + """
int main() {
  {
    auto first = std::make_shared<Record>();
    {
      auto second = first.clone();
      if (second->read_view().read() != 7 || second->root_view().root_value() != 31) { return 1; }
    }
    if (events != 12 || first->leaf.read() != 7) { return 2; }
  }
  return events == 1234 ? 0 : 3;
}
""", endpoints=TRANSFER_ENDPOINTS, cpp_limitation=CPP_LIMITATIONS["shared-owner-reference"])
    add("reject-source-reset-retained-view", reference_prefix + """
int main() {
  mut auto owner = std::make_unique<Record>();
  Reader& view = owner->read_view();
  owner.reset();
  return view.read();
}
""", "source-rejection")
    owner_failure = reference_prefix + """
class Guard {
public:
  ~Guard() {
    if (events != 1234) { int divisor = 0; int checked_cleanup = 1 / divisor; }
  }
};
int main() {
  Guard guard{};
  auto owner = std::make_unique<Record>();
  return owner->leaf.fail(0);
}
"""
    add("owner-reference-failure", owner_failure, "defined-failure", expected_exit=70,
        failure_needle="return 120 / divisor;", endpoints=TRANSFER_ENDPOINTS,
        cpp_limitation=CPP_LIMITATIONS["owner-reference-failure"])
    add("owner-failure-before-reference", owner_failure.replace(
        "return owner->leaf.fail(0);", "return owner->checked_view(0).read();"),
        "defined-failure", expected_exit=70, failure_needle="checked_before_view = 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS, cpp_limitation=CPP_LIMITATIONS["owner-failure-before-reference"])
    add("owner-reference-wrong-cleanup", owner_failure.replace("events != 1234", "events != 1235"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="return 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS[2:])
    if source.count(ALIAS_ANCHOR) != 1:
        raise AssertionError("borrowed-view anchor is not unique")
    add("borrowed-dispatch", source.replace(ALIAS_ANCHOR, ALIAS_ANCHOR + '\n    {\n      Reader& view = value;\n      if (view.read(1) != 11) { return 6; }\n      if (via_reader(view, 2) != 12) { return 7; }\n      Reader& second = view;\n      if (via_reader(second, 3) != 13) { return 8; }\n    }\n    {\n      mut Root& root_view = value;\n      if (root_view.base_value() != 7) { return 9; }\n      root_view.set_base(9);\n      if (root_view.base_value() != 9) { return 10; }\n      root_view.set_base(7);\n    }\n    {\n      mut Writer& view = value;\n      via_writer(view, 23);\n    }\n    if (!via_truth(value, true)) { return 11; }\n'))
    add("borrowed-mut-reference-chain", source.replace(ALIAS_ANCHOR, ALIAS_ANCHOR + '\n    {\n      mut Writer& view = value;\n      { mut Writer& second = view; via_writer(second, 23); }\n    }\n    if (!via_truth(value, true)) { return 6; }\n'),
        cpp_limitation=CPP_LIMITATIONS["borrowed-mut-reference-chain"])
    lifecycle_origin = "tests/fixtures/llvm_dispatch_execution_lifecycle.gti"
    lifecycle = (root / lifecycle_origin).read_text(encoding="utf-8")
    marker = "// DISPATCH_LIFECYCLE_MAIN\n"
    if lifecycle.count(marker) != 1:
        raise AssertionError("lifecycle main marker is not unique")
    add("virtual-failure-cleanup", lifecycle, "defined-failure", lifecycle_origin, 70,
        "return 120 / divisor;")
    move_anchor = "  Leaf value{7};"
    if lifecycle.count(move_anchor) != 1:
        raise AssertionError("fresh move failure anchor is not unique")
    add("generated-move-virtual-failure",
        lifecycle.replace(move_anchor, "  Leaf original{7};\n  Leaf value = std::move(original);"),
        "defined-failure", expected_exit=70, failure_needle="return 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS)
    add("constructor-rollback", lifecycle.split(marker, 1)[0] + ROLLBACK_MAIN,
        "defined-failure", expected_exit=70, failure_needle="quotient(12 / divisor)")
    prefix = lifecycle.split(marker, 1)[0]
    add("inline-field-virtual-failure", prefix + """
struct Holder { mut Leaf leaf{7}; };
int main() {
  Guard guard{};
  Holder holder{};
  return invoke_failure(holder.leaf, 0);
}
""", "defined-failure", expected_exit=70, failure_needle="return 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS, cpp_limitation=CPP_LIMITATIONS["inline-field-virtual-failure"])
    add("inline-field-constructor-rollback", prefix + """
struct Holder {
  mut Leaf first{7};
  mut Leaf second;
  Holder() : second(Leaf(0)) {}
  ~Holder() { record(9); }
};
int main() {
  expected_cleanup = 1231221321;
  Guard guard{};
  Holder holder{};
  return 0;
}
""", "defined-failure", expected_exit=70, failure_needle="quotient(12 / divisor)",
        endpoints=TRANSFER_ENDPOINTS, cpp_limitation=CPP_LIMITATIONS["inline-field-constructor-rollback"])
    add("inline-field-wrong-cleanup-trace",
        cases["inline-field-constructor-rollback"].source_text.replace(
            "expected_cleanup = 1231221321;", "expected_cleanup = 1231221322;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="quotient(12 / divisor)",
        endpoints=TRANSFER_ENDPOINTS[2:])
    anchor = "mut int expected_cleanup = 1234321;"
    if lifecycle.count(anchor) != 1:
        raise AssertionError("cleanup oracle mutation anchor is not unique")
    add("cleanup-oracle-wrong-trace", lifecycle.replace(anchor, "mut int expected_cleanup = 1234322;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="return 120 / divisor;")
    owners = (root / SUCCESS_FILES["polymorphic-owners"]).read_text(encoding="utf-8")
    owners_marker = "// POLYMORPHIC_OWNERS_MAIN\n"
    if owners.count(owners_marker) != 1:
        raise AssertionError("polymorphic owner main marker is not unique")
    owners_prefix = owners.split(owners_marker, 1)[0]
    owner_failure = owners_prefix + """int main() {
  expected_cleanup = 1263549;
  Guard guard{};
  std::unique_ptr<Reader> first = make_reader(false);
  std::unique_ptr<Root> owner = std::upcast_unique<Root, Leaf>(std::make_unique<Leaf>(6));
  return invoke_failure(*owner, 0);
}
"""
    add("polymorphic-owner-virtual-failure", owner_failure, "defined-failure",
        expected_exit=70, failure_needle="return 120 / divisor;", endpoints=TRANSFER_ENDPOINTS)
    add("polymorphic-owner-wrong-cleanup",
        owner_failure.replace("expected_cleanup = 1263549;", "expected_cleanup = 1263548;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="return 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS)
    add("polymorphic-owner-constructor-rollback", owners_prefix + """int main() {
  expected_cleanup = 149;
  Guard guard{};
  std::unique_ptr<Reader> first = make_reader(false);
  std::unique_ptr<Reader> failed = std::upcast_unique<Reader, Leaf>(std::make_unique<Leaf>(0));
  return 0;
}
""", "defined-failure", expected_exit=70, failure_needle="payload(12 / payload)",
        endpoints=TRANSFER_ENDPOINTS)
    signatures = (root / SUCCESS_FILES["virtual-signatures"]).read_text(encoding="utf-8")
    signatures_marker = "// VIRTUAL_SIGNATURES_MAIN\n"
    if signatures.count(signatures_marker) != 1:
        raise AssertionError("virtual signature main marker is not unique")
    signature_failure = signatures.split(signatures_marker, 1)[0] + """int main() {
  expected_cleanup = 3;
  Guard guard{};
  mut Box box{};
  Tracked held{3};
  Shape& shape = box;
  Point failed = shape.corner(0);
  return failed.x;
}
"""
    signature_needle = "return Point(this.origin.x * 12 / scale"
    add("virtual-signature-failure", signature_failure, "defined-failure",
        expected_exit=70, failure_needle=signature_needle, endpoints=TRANSFER_ENDPOINTS)
    add("virtual-signature-wrong-cleanup",
        signature_failure.replace("expected_cleanup = 3;", "expected_cleanup = 4;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=signature_needle,
        endpoints=TRANSFER_ENDPOINTS)
    publication = (root / SUCCESS_FILES["fresh-publication"]).read_text(encoding="utf-8")
    publication_marker = "// FRESH_PUBLICATION_MAIN\n"
    if publication.count(publication_marker) != 1:
        raise AssertionError("fresh publication main marker is not unique")
    publication_prefix = publication.split(publication_marker, 1)[0]
    temporary_failure = publication_prefix + """int main() {
  expected_cleanup = 12634;
  Guard guard{};
  return Leaf(2).fail(0);
}
"""
    add("fresh-temporary-virtual-failure", temporary_failure, "defined-failure",
        expected_exit=70, failure_needle="return 120 / divisor;", endpoints=TRANSFER_ENDPOINTS)
    add("fresh-temporary-wrong-cleanup",
        temporary_failure.replace("expected_cleanup = 12634;", "expected_cleanup = 12635;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle="return 120 / divisor;",
        endpoints=TRANSFER_ENDPOINTS)
    add("fresh-return-constructor-rollback", publication_prefix + """int main() {
  expected_cleanup = 121434;
  Guard guard{};
  Leaf held = make(3);
  Leaf failed = make(0);
  return failed.read() + held.read();
}
""", "defined-failure", expected_exit=70, failure_needle="payload(12 / payload)",
        endpoints=TRANSFER_ENDPOINTS)
    slots = (root / SUCCESS_FILES["inline-slots"]).read_text(encoding="utf-8")
    slots_marker = "// INLINE_SLOTS_MAIN\n"
    if slots.count(slots_marker) != 1:
        raise AssertionError("inline slot main marker is not unique")
    slot_failure = slots.split(slots_marker, 1)[0] + """int main() {
  expected_cleanup = 3;
  Guard guard{};
  mut GaugeSlot boxed{};
  boxed.replace(Logged(std::string("ab"), 3));
  mut GaugeSlot faulty{};
  faulty.replace(Faulty(0));
  return drive(faulty, 1) + drive(boxed, 1);
}
"""
    slot_needle = "int sample(int tick) mut override { return 120 / this.divisor; }"
    add("inline-slot-virtual-failure", slot_failure, "defined-failure",
        expected_exit=70, failure_needle=slot_needle, endpoints=TRANSFER_ENDPOINTS)
    add("inline-slot-wrong-cleanup",
        slot_failure.replace("expected_cleanup = 3;", "expected_cleanup = 4;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=slot_needle,
        endpoints=TRANSFER_ENDPOINTS)
    owned = (root / SUCCESS_FILES["owned-transfers"]).read_text(encoding="utf-8")
    owned_marker = "// OWNED_TRANSFERS_MAIN\n"
    if owned.count(owned_marker) != 1:
        raise AssertionError("owned transfer main marker is not unique")
    owned_failure = owned.split(owned_marker, 1)[0] + """int main() {
  expected_cleanup = 54;
  Guard guard{};
  Badge held = Badge(std::string("x"), 4);
  return divide(Badge(std::string("yy"), 5), 0) + held.code();
}
"""
    owned_needle = "int divide(Badge badge, int divisor) { return badge.code() / divisor; }"
    add("owned-transfer-failure", owned_failure, "defined-failure",
        expected_exit=70, failure_needle=owned_needle, endpoints=TRANSFER_ENDPOINTS)
    add("owned-transfer-wrong-cleanup",
        owned_failure.replace("expected_cleanup = 54;", "expected_cleanup = 55;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=owned_needle,
        endpoints=TRANSFER_ENDPOINTS)
    consuming = (root / SUCCESS_FILES["consuming-receivers"]).read_text(encoding="utf-8")
    consuming_marker = "// CONSUMING_RECEIVERS_MAIN\n"
    if consuming.count(consuming_marker) != 1:
        raise AssertionError("consuming receiver main marker is not unique")
    consuming_failure = consuming.split(consuming_marker, 1)[0] + """int main() {
  expected_cleanup = 68991;
  Guard guard{};
  Task held = Task(1, std::string("x"), 1);
  Runner runner = Runner(boxed(6, 0), 700);
  return std::move(runner).run(8) + held.rank();
}
"""
    consuming_needle = "return (input + taken.read() * 10 + int(this.label.size())) / this.divisor;"
    add("consuming-receiver-failure", consuming_failure, "defined-failure",
        expected_exit=70, failure_needle=consuming_needle, endpoints=TRANSFER_ENDPOINTS)
    add("consuming-receiver-wrong-cleanup",
        consuming_failure.replace("expected_cleanup = 68991;", "expected_cleanup = 68992;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=consuming_needle,
        endpoints=TRANSFER_ENDPOINTS)
    closures = (root / SUCCESS_FILES["closure-models"]).read_text(encoding="utf-8")
    closures_marker = "// CLOSURE_MODELS_MAIN\n"
    if closures.count(closures_marker) != 1:
        raise AssertionError("closure model main marker is not unique")
    closure_failure = closures.split(closures_marker, 1)[0] + """int main() {
  expected_cleanup = 748;
  Guard guard{};
  auto first = std::make_unique<Token>(8);
  std::unique_ptr<Callback> kept = boxed(
      [owned = std::move(first)](int input) -> int { return input + owned->read(); });
  auto second = std::make_unique<Token>(4);
  auto held = retained(
      [owned = std::move(second)](int input) -> int { return 840 / (input - owned->read()); });
  Token last = Token(7);
  Callback& view = held;
  return view.call(4) + kept->call(1) + last.read();
}
"""
    closure_needle = "return 840 / (input - owned->read());"
    add("closure-model-failure", closure_failure, "defined-failure",
        expected_exit=70, failure_needle=closure_needle, endpoints=TRANSFER_ENDPOINTS)
    add("closure-model-wrong-cleanup",
        closure_failure.replace("expected_cleanup = 748;", "expected_cleanup = 749;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=closure_needle,
        endpoints=TRANSFER_ENDPOINTS)
    phases = (root / SUCCESS_FILES["abstract-phases"]).read_text(encoding="utf-8")
    phases_marker = "// ABSTRACT_PHASES_MAIN\n"
    if phases.count(phases_marker) != 1:
        raise AssertionError("abstract phase main marker is not unique")
    phase_failure = phases.split(phases_marker, 1)[0] + """int main() {
  expected_cleanup = 12384;
  Guard guard{};
  Square square = Square(3);
  return square.ratio(0);
}
"""
    phase_needle = "int ratio(int divisor) { return this.area() / divisor; }"
    add("abstract-phase-failure", phase_failure, "defined-failure",
        expected_exit=70, failure_needle=phase_needle, endpoints=TRANSFER_ENDPOINTS)
    add("abstract-phase-wrong-cleanup",
        phase_failure.replace("expected_cleanup = 12384;", "expected_cleanup = 12385;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=phase_needle,
        endpoints=TRANSFER_ENDPOINTS)
    temporaries = (root / SUCCESS_FILES["temporary-receivers"]).read_text(encoding="utf-8")
    temporaries_marker = "// TEMPORARY_RECEIVERS_MAIN\n"
    if temporaries.count(temporaries_marker) != 1:
        raise AssertionError("temporary receiver main marker is not unique")
    temporary_failure = temporaries.split(temporaries_marker, 1)[0] + """int main() {
  expected_cleanup = 62;
  Guard guard{};
  Source kept = Source(2);
  return joined(false, 0) + kept.share(1);
}
"""
    temporary_needle = "virtual int share(int divisor) { return 60 / divisor; }"
    add("temporary-receiver-failure", temporary_failure, "defined-failure",
        expected_exit=70, failure_needle=temporary_needle, endpoints=TRANSFER_ENDPOINTS)
    add("temporary-receiver-wrong-cleanup",
        temporary_failure.replace("expected_cleanup = 62;", "expected_cleanup = 63;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=temporary_needle,
        endpoints=TRANSFER_ENDPOINTS)
    replacements = (root / SUCCESS_FILES["replacements"]).read_text(encoding="utf-8")
    replacements_marker = "// REPLACEMENTS_MAIN\n"
    if replacements.count(replacements_marker) != 1:
        raise AssertionError("replacement main marker is not unique")
    replacement_failure = replacements.split(replacements_marker, 1)[0] + """int main() {
  expected_cleanup = 72;
  Guard guard{};
  mut Square square = Square(2);
  Square kept = Square(7);
  square = make(0);
  return square.area() + kept.area();
}
"""
    replacement_needle = "Square make(int side) { return Square(12 / side); }"
    add("replacement-failure", replacement_failure, "defined-failure",
        expected_exit=70, failure_needle=replacement_needle, endpoints=TRANSFER_ENDPOINTS)
    add("replacement-wrong-cleanup",
        replacement_failure.replace("expected_cleanup = 72;", "expected_cleanup = 73;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=replacement_needle,
        endpoints=TRANSFER_ENDPOINTS)
    for name, (source, expected_exit) in LIFECYCLE_FORMER_REJECTS.items():
        add(name, source, expected_exit=expected_exit, endpoints=TRANSFER_ENDPOINTS)
    add("rollback-virtual-observation-former-reject", LIFECYCLE_ROLLBACK_FORMER_REJECT,
        "defined-failure", expected_exit=70,
        failure_needle="Derived(int divisor) : Base(), quotient(12 / divisor) {}",
        endpoints=TRANSFER_ENDPOINTS)
    lifecycle = (root / SUCCESS_FILES["lifecycle-phases"]).read_text(encoding="utf-8")
    lifecycle_marker = "// LIFECYCLE_MAIN\n"
    if lifecycle.count(lifecycle_marker) != 1:
        raise AssertionError("lifecycle main marker is not unique")
    lifecycle_rollback = lifecycle.split(lifecycle_marker, 1)[0] + """int main() {
  expected_cleanup = 241241;
  Guard guard{};
  Mid kept{};
  Leaf failed{0};
  return failed.read() + kept.read();
}
"""
    lifecycle_needle = "Leaf(int divisor) : Mid(), quotient(12 / divisor) {}"
    add("lifecycle-phase-rollback", lifecycle_rollback, "defined-failure",
        expected_exit=70, failure_needle=lifecycle_needle, endpoints=TRANSFER_ENDPOINTS)
    add("lifecycle-phase-wrong-cleanup",
        lifecycle_rollback.replace("expected_cleanup = 241241;", "expected_cleanup = 241242;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=lifecycle_needle,
        endpoints=TRANSFER_ENDPOINTS)
    shared = (root / SUCCESS_FILES["shared-polymorphic-owners"]).read_text(encoding="utf-8")
    shared_marker = "// SHARED_OWNERS_MAIN\n"
    if shared.count(shared_marker) != 1:
        raise AssertionError("shared owner main marker is not unique")
    shared_failure = shared.split(shared_marker, 1)[0] + """int main() {
  expected_cleanup = 76;
  Guard guard{};
  auto keep = std::make_shared<Square>(6);
  std::shared_ptr<Shape> shape =
      std::upcast_shared<Shape, Square>(std::make_shared<Square>(7));
  return shape->area(0) + keep->code();
}
"""
    shared_needle = "int area(int scale) override { return this.side * this.side / scale; }"
    add("shared-polymorphic-owner-failure", shared_failure, "defined-failure",
        expected_exit=70, failure_needle=shared_needle, endpoints=TRANSFER_ENDPOINTS)
    add("shared-polymorphic-owner-wrong-cleanup",
        shared_failure.replace("expected_cleanup = 76;", "expected_cleanup = 77;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=shared_needle,
        endpoints=TRANSFER_ENDPOINTS)
    storage = (root / SUCCESS_FILES["program-storage"]).read_text(encoding="utf-8")
    storage_marker = "// PROGRAM_STORAGE_MAIN\n"
    if storage.count(storage_marker) != 1:
        raise AssertionError("program storage main marker is not unique")
    add("program-storage-failure", storage.split(storage_marker, 1)[0] + """Square broken = scaled(0);

int main() { return broken.area() + square.area(); }
""", "defined-failure", expected_exit=70,
        failure_needle="Square scaled(int divisor) { return Square(12 / divisor); }",
        endpoints=TRANSFER_ENDPOINTS)
    arrays = (root / SUCCESS_FILES["hierarchy-arrays"]).read_text(encoding="utf-8")
    arrays_marker = "// HIERARCHY_ARRAYS_MAIN\n"
    if arrays.count(arrays_marker) != 1:
        raise AssertionError("hierarchy array main marker is not unique")
    arrays_prefix = arrays.split(arrays_marker, 1)[0]
    array_failure = arrays_prefix + """int main() {
  expected_cleanup = 14;
  Guard guard{};
  Square kept = Square(4);
  Square squares[3] = {Square(1), make(0), Square(3)};
  return squares[0].area() + kept.area();
}
"""
    array_field_failure = arrays_prefix + """int main() {
  expected_cleanup = 54;
  Guard guard{};
  Square kept = Square(4);
  Rack rack = Rack(5, 0);
  return rack.row[0].area() + kept.area();
}
"""
    array_needle = "Square make(int side) { return Square(12 / side); }"
    add("hierarchy-array-failure", array_failure, "defined-failure",
        expected_exit=70, failure_needle=array_needle, endpoints=TRANSFER_ENDPOINTS)
    add("hierarchy-array-wrong-cleanup",
        array_failure.replace("expected_cleanup = 14;", "expected_cleanup = 15;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=array_needle,
        endpoints=TRANSFER_ENDPOINTS)
    add("hierarchy-array-field-failure", array_field_failure, "defined-failure",
        expected_exit=70, failure_needle=array_needle, endpoints=TRANSFER_ENDPOINTS)
    add("hierarchy-array-field-wrong-cleanup",
        array_field_failure.replace("expected_cleanup = 54;", "expected_cleanup = 55;"),
        "wrong-cleanup-trace", expected_exit=70, failure_needle=array_needle,
        endpoints=TRANSFER_ENDPOINTS)
    for name, source in REJECTIONS.items():
        add(name, source, "backend-rejection")
    for name, source in SOURCE_REJECTIONS.items():
        add(name, source, "source-rejection")
    return cases


def evidence_cases(root: Path) -> dict[str, str]:
    return {name: case.source_text for name, case in dispatch_cases(root).items() if not case.is_control}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    profiles = {
        "base-interface-adjustments": ("static-dispatch", "shipped-example",
                                       "inline-hierarchy-fields", "nested-object-former-reject",
                                       "inline-field-virtual-failure", "inline-field-constructor-rollback",
                                       "inline-field-wrong-cleanup-trace",
                                       "abstract-phases", "abstract-phase-failure",
                                       "abstract-phase-wrong-cleanup",
                                       "pure-active-phase-former-reject",
                                       "destructor-virtual-observation-former-reject",
                                       "destructor-helper-observation-former-reject",
                                       "read-borrow-cleanup-observation-former-reject",
                                       "mut-borrow-cleanup-observation-former-reject",
                                       "owned-destructor-observation-former-reject",
                                       "cleanup-helper-cycle-observation-former-reject",
                                       "stored-interface-reference"),
        "mut-borrow-scalar-bool": ("borrowed-dispatch", "borrowed-mut-reference-chain",
                                  "hierarchy-reference-results", "owner-reference-failure",
                                  "owner-reference-wrong-cleanup", "shared-owner-reference",
                                  "owner-failure-before-reference", "replacements"),
        "virtual-invocation": ("fresh-local-virtual-call", "scalar-state-virtual-control",
                               "static-dispatch", "dispatch-cost-correctness",
                               "virtual-signatures", "virtual-signature-failure",
                               "virtual-signature-wrong-cleanup", "temporary-receivers",
                               "temporary-receiver-failure",
                               "temporary-receiver-wrong-cleanup"),
        "dispatch-lifecycle": ("virtual-failure-cleanup", "constructor-rollback",
                               "cleanup-oracle-wrong-trace", "nonobserving-cleanup-helper-cycle"),
        "fresh-local-generated-transfers": (
            "fresh-hierarchy-transfer", "fresh-hierarchy-copy-view",
            "nested-hierarchy-transfer", "nested-transfer-failure", "nested-transfer-wrong-cleanup",
            "generated-copy-former-reject", "generated-move-former-reject",
            "fresh-copy-former-cli-control", "generated-move-virtual-failure",
            "owned-transfers", "owned-transfer-failure", "owned-transfer-wrong-cleanup",
            "local-return-former-reject", "returned-owned-move"),
        "fresh-publication": (
            "fresh-publication", "fresh-return-former-reject",
            "fresh-value-argument-former-reject",
            "fresh-temporary-virtual-failure", "fresh-temporary-wrong-cleanup",
            "fresh-return-constructor-rollback"),
        "inline-polymorphic-slot": (
            "inline-slots", "inline-slot-virtual-failure", "inline-slot-wrong-cleanup",
            "field-default-closure-former-reject"),
        "consuming-receivers": (
            "consuming-receivers", "consuming-receiver-failure",
            "consuming-receiver-wrong-cleanup"),
        "hierarchy-replacement": (
            "replacements", "replacement-failure", "replacement-wrong-cleanup",
            "assignment-former-reject"),
        "hierarchy-arrays": (
            "hierarchy-arrays", "hierarchy-array-failure", "hierarchy-array-wrong-cleanup",
            "hierarchy-array-field-failure", "hierarchy-array-field-wrong-cleanup",
            "array-former-reject"),
        "program-hierarchy-storage": (
            "program-storage", "program-storage-failure", "global-former-reject"),
        "lifecycle-dispatch": (
            "lifecycle-phases", "lifecycle-phase-rollback", "lifecycle-phase-wrong-cleanup",
            "base-destructor-observation-former-reject",
            "base-helper-observation-former-reject",
            "cleanup-helper-cycle-base-phase-former-reject",
            "rollback-virtual-observation-former-reject"),
        "polymorphic-ownership": (
            "polymorphic-owners", "heap-polymorphic-owner-former-reject",
            "polymorphic-owner-virtual-failure", "polymorphic-owner-wrong-cleanup",
            "polymorphic-owner-constructor-rollback", "closure-models",
            "closure-model-failure", "closure-model-wrong-cleanup",
            "shared-polymorphic-owners", "shared-polymorphic-owner-failure",
            "shared-polymorphic-owner-wrong-cleanup"),
    }
    cases = evidence_cases(root)
    if {name for members in profiles.values() for name in members} != set(cases):
        raise AssertionError("dispatch profiles differ from default positive execution sources")
    return profiles


def evidence_manifest(root: Path) -> dict:
    origins = {name: case.context.source for name, case in dispatch_cases(root).items()
               if not case.is_control and case.context.source_kind == "repository"}
    return build_manifest("llvm_dispatch_execution", evidence_cases(root), evidence_profiles(root), origins)


def evidence_partition_policy(root: Path) -> dict[str, str]:
    del root
    return {"cases": "positive-admission", "controls": "expected-rejection-control"}


def evidence_context(root: Path) -> dict:
    cases = dispatch_cases(root)
    return build_context_sidecar(
        evidence_manifest(root),
        {name: case.sidecar_record() for name, case in cases.items() if not case.is_control},
        {name: case.sidecar_record() for name, case in cases.items() if case.is_control},
        partition_policy=evidence_partition_policy(root))


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    names = (*SUCCESS_FILES, *SUCCESS_SOURCES, *TRANSFER_SUCCESS_SOURCES, "borrowed-dispatch", "borrowed-mut-reference-chain",
             "virtual-failure-cleanup", "generated-move-virtual-failure", "constructor-rollback", "cleanup-oracle-wrong-trace",
             "inline-field-virtual-failure", "inline-field-constructor-rollback",
             "inline-field-wrong-cleanup-trace", *REJECTIONS,
             "owner-reference-failure", "owner-reference-wrong-cleanup",
             "shared-owner-reference", "reject-source-reset-retained-view",
             "owner-failure-before-reference",
             "nested-transfer-failure", "nested-transfer-wrong-cleanup",
             "polymorphic-owner-virtual-failure", "polymorphic-owner-wrong-cleanup",
             "polymorphic-owner-constructor-rollback",
             "virtual-signature-failure", "virtual-signature-wrong-cleanup",
             "fresh-temporary-virtual-failure", "fresh-temporary-wrong-cleanup",
             "fresh-return-constructor-rollback",
             "inline-slot-virtual-failure", "inline-slot-wrong-cleanup",
             "owned-transfer-failure", "owned-transfer-wrong-cleanup",
             "consuming-receiver-failure", "consuming-receiver-wrong-cleanup",
             "closure-model-failure", "closure-model-wrong-cleanup",
             "abstract-phase-failure", "abstract-phase-wrong-cleanup",
             "temporary-receiver-failure", "temporary-receiver-wrong-cleanup",
             "replacement-failure", "replacement-wrong-cleanup",
             "hierarchy-array-failure", "hierarchy-array-wrong-cleanup",
             "hierarchy-array-field-failure", "hierarchy-array-field-wrong-cleanup",
             "program-storage-failure",
             "shared-polymorphic-owner-failure", "shared-polymorphic-owner-wrong-cleanup",
             *LIFECYCLE_FORMER_REJECTS, "rollback-virtual-observation-former-reject",
             "lifecycle-phase-rollback", "lifecycle-phase-wrong-cleanup",
             *SOURCE_REJECTIONS)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--case", action="append", choices=names, dest="cases")
    parser.add_argument("--measure", type=Path, metavar="EVIDENCE_DIR",
                        help="opt-in: prepare persistent artifacts and STOP; separate review+host approval unlock sampling")
    parser.add_argument("--measure-review", type=Path, help="independent artifact-bound coordinator review JSON")
    parser.add_argument("--measure-host-plan", type=Path, help="separate approved uncontended-host sampling window JSON")
    parser.add_argument("--measure-count", type=int, default=8_000_000)
    parser.add_argument("--measure-samples", type=int, default=9)
    parser.add_argument("--measure-target-seconds", type=float, default=0.5)
    parser.add_argument("--measure-max-batch", type=int, default=32)
    arguments = parser.parse_args()
    gti, root = arguments.gti.resolve(), arguments.root.resolve()
    selected = set(arguments.cases or names)
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    if arguments.measure:
        measure_main(gti, root, env, arguments)
        return
    if arguments.measure_review or arguments.measure_host_plan:
        parser.error("--measure-review/--measure-host-plan require --measure")
    specifications = dispatch_cases(root)
    if set(specifications) != set(names):
        raise AssertionError("dispatch selection differs from its shared case authority")
    with tempfile.TemporaryDirectory(prefix="gti-llvm-dispatch-") as temporary:
        work = Path(temporary)
        for name, specification in specifications.items():
            if name not in selected:
                continue
            if specification.kind == "backend-rejection":
                reject_case(gti, root, work, name, specification.source_text, env)
                continue
            if specification.kind == "source-rejection":
                case = work / name
                case.mkdir()
                source = case / specification.context.materialized_name
                source.write_text(specification.source_text, encoding="utf-8")
                for mode in ("O0", "O3"):
                    output = case / f"program-{mode}"
                    sentinel = b"existing source-rejection artifact\n"
                    output.write_bytes(sentinel)
                    result = invoke([str(gti), str(source), "--backend", "llvm",
                                     f"-{mode}", "-o", str(output)], root, env)
                    if (result.returncode != 65 or "GTI-S" not in result.stderr
                            or (name in SOURCE_REJECTION_CODES and
                                SOURCE_REJECTION_CODES[name] not in result.stderr)
                            or "GTI-B0003" in result.stderr
                            or output.read_bytes() != sentinel):
                        raise AssertionError(f"{name} {mode}: {result}")
                print(f"{name}: source rejection and artifact preservation passed", flush=True)
                continue
            if specification.context.source_kind == "repository":
                source = root / specification.context.source
                if source.read_text(encoding="utf-8") != specification.source_text:
                    raise AssertionError(f"dispatch source changed during execution: {source}")
            else:
                source = work / specification.context.materialized_name
                source.write_text(specification.source_text, encoding="utf-8")
            if specification.kind == "wrong-cleanup-trace":
                line = next(index for index, value in enumerate(specification.source_text.splitlines(), 1)
                            if specification.failure_needle in value)
                for endpoint in specification.endpoints:
                    case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard or 'native'}"
                    result = execute(gti, root, case, source, endpoint, env)
                    divergence = specification.cpp_divergence
                    if divergence and endpoint.backend == "cpp":
                        if (result.error or result.returncode != divergence[0]
                                or divergence[1] not in result.stderr):
                            raise AssertionError(f"{name}: pinned C++ divergence changed: {result}")
                        continue
                    if (result.error or result.returncode != 70 or result.stdout
                            or "GTI-R0014" not in result.stderr
                            or "primary [GTI-R0002]" not in result.stderr
                            or f"{source.name}:{line}" not in result.stderr
                            or expected_failure(result, source, line)):
                        raise AssertionError(f"controlled wrong trace failed to discriminate: {endpoint}: {result}")
                print(f"{name}: secondary failure reproduced; positive oracle rejects the mutation", flush=True)
            else:
                execute_case(gti, root, work, name, source, env,
                             expected_exit=specification.expected_exit,
                             failure_needle=specification.failure_needle,
                             cpp_limitation=specification.cpp_limitation,
                             endpoints=specification.endpoints,
                             cpp_divergence=specification.cpp_divergence)


if __name__ == "__main__":
    main()
