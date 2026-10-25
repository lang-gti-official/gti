#!/usr/bin/env python3
"""Execute the bounded static-local dispatch slice and reject object transfers.

The cost fixture is a correctness workload, not a timing/dispatch/allocation
proof. Performance measurement belongs to the coordinated platform host plan.
The verified constructor-state plan and explicit destructor IDs establish the
positive traces. Virtual observations during destruction/rollback remain
rejections pending an explicit backend-neutral phase contract. No C++ result
defines that missing contract, and no proposed phase trace is accepted here.
The nested mutable-reference chain is source/MIR-valid and native-required;
both existing C++ text-vocabulary rejections are recorded, not counted as passes.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from backend_execution import Endpoint, execute


ENDPOINTS = (
    Endpoint("cpp", "O3", "c++20"),
    Endpoint("cpp", "O3", "c++23"),
    Endpoint("llvm", "O0"),
    Endpoint("llvm", "O3"),
)

SUCCESS_FILES = {
    "static-dispatch": "tests/fixtures/llvm_dispatch_execution.gti",
    "dispatch-cost-correctness": "tests/fixtures/llvm_dispatch_execution_cost.gti",
    "shipped-example": "examples/llvm/inheritance-and-dispatch.gti",
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

REJECTIONS = {
    "reject-copy": PLAIN + "int main() { Plain first{}; Plain second = first; return second.read(); }\n",
    "reject-move": PLAIN + "int main() { mut Plain first{}; Plain second = std::move(first); return second.read(); }\n",
    "reject-move-construction": PLAIN + "int main() { mut Plain first{}; Plain second = Plain(std::move(first)); return second.read(); }\n",
    "reject-assignment": PLAIN + "int main() { mut Plain first{}; Plain second{}; first = second; return first.read(); }\n",
    "reject-base-assignment": PLAIN + """class Derived : public Plain {};
void replace(mut Plain& destination, Plain& source) { destination = source; }
int main() { mut Derived first{}; Derived second{}; replace(first, second); return first.read(); }
""",
    "reject-fresh-value-argument": PLAIN + "void take(Plain value) {}\nint main() { take(Plain()); return 0; }\n",
    "reject-fresh-return": PLAIN + "Plain make() { return Plain(); }\nint main() { Plain value = make(); return value.read(); }\n",
    "reject-array": PLAIN + "int main() { Plain values[1] = {Plain()}; return values[0].read(); }\n",
    "reject-nested-object": PLAIN + "struct Wrapper { Plain value = Plain(); };\nint main() { Wrapper value{}; return value.value.read(); }\n",
    "reject-global": PLAIN + "Plain value{};\nint main() { return value.read(); }\n",
    "reject-thread-local": PLAIN + "thread_local Plain value{};\nint main() { return value.read(); }\n",
    "reject-pure-active-phase": """class Base {
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
""",
    "reject-destructor-virtual-observation": """class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  ~Observed() { int observed = this.read(); }
};
int main() { Observed value{}; return value.read() - 1; }
""",
    "reject-destructor-helper-observation": """class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  int observe() { return this.read(); }
  ~Observed() { int observed = this.observe(); }
};
int main() { Observed value{}; return value.read() - 1; }
""",
    "reject-rollback-virtual-observation": """class Base {
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
""",
    "reject-read-borrow-cleanup-observation": """class Observed {
public:
  Observed() {}
  virtual int read() { return 1; }
  static int through(Observed& value) { return value.read(); }
  ~Observed() { int observed = Observed::through(this); }
};
int main() { Observed value{}; return 0; }
""",
    "reject-mut-borrow-cleanup-observation": """class Observed {
public:
  mut int value = 0;
  Observed() {}
  virtual void touch() mut { this.value += 1; }
  static void through(mut Observed& value) { value.touch(); }
  ~Observed() { Observed::through(this); }
};
int main() { Observed value{}; return 0; }
""",
    "reject-heap-polymorphic-owner": """#include <std/memory>
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
""",
}

# The same terminating call cycle must not hide its reachable virtual observer.
REJECTIONS["reject-cleanup-helper-cycle-observation"] = SUCCESS_SOURCES[
    "nonobserving-cleanup-helper-cycle"].replace(
        "return value.payload();", "return value.read();")

SOURCE_REJECTIONS = {
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
                 cpp_vocabulary_limitation: bool = False) -> None:
    observables = []
    if cpp_vocabulary_limitation:
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
    for endpoint in ENDPOINTS:
        case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard or 'native'}"
        result = execute(gti, root, case, source, endpoint, env)
        if cpp_vocabulary_limitation and endpoint.backend == "cpp":
            match = re.search(r"backend diagnostics saved at ([^\n]+)", result.stderr)
            detail = Path(match[1]).read_text(encoding="utf-8") if match else ""
            if (result.compile_returncode != 65 or "GTI-B0001" not in result.stderr
                    or "outside the complete C++ text vocabulary" not in detail):
                raise AssertionError(f"{name}: C++ comparator status changed; review coverage: {result}")
            print(f"{name} {endpoint.standard}: C++ comparator REJECTED (not execution evidence)", flush=True)
            print(f"command: {result.command}\n{result.stderr}\n{detail}", flush=True)
            continue
        if result.error:
            raise AssertionError(f"{name} {endpoint}: {result}")
        if failure_needle is None:
            if result.observable != (0, "", ""):
                raise AssertionError(f"{name} {endpoint}: {result}")
        elif not expected_failure(result, source, failure_line):
            raise AssertionError(f"{name} {endpoint}: {result}")
        observables.append(result.observable)
    if len(set(observables)) != 1:
        raise AssertionError(f"{name}: backend observable/source-attribution drift: {observables}")
    if cpp_vocabulary_limitation:
        print(f"{name}: verified source/MIR and native O0/O3 passed; C++ failures retained", flush=True)
    else:
        print(f"{name}: C++20/23 O3 and native O0/O3 passed", flush=True)


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
                        else "PureDispatchState" if name == "reject-pure-active-phase"
                        else None if name in {"reject-global", "reject-thread-local", "reject-heap-polymorphic-owner"}
                        else "PolymorphicTransfer")
            if expected and f"Shape.{expected}" not in result.stderr:
                raise AssertionError(f"{name}: rejection did not exercise {expected}: {result}")
            if list(case.rglob("*.o")) or list(case.rglob("*.obj")):
                raise AssertionError(f"{name}: rejected native source published an object")
    print(f"{name}: verified source, native O0/O3 rejection and artifact preservation passed", flush=True)


def main() -> None:
    names = (*SUCCESS_FILES, *SUCCESS_SOURCES, "borrowed-dispatch", "borrowed-mut-reference-chain",
             "virtual-failure-cleanup", "constructor-rollback", "cleanup-oracle-wrong-trace", *REJECTIONS,
             *SOURCE_REJECTIONS)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--case", action="append", choices=names, dest="cases")
    arguments = parser.parse_args()
    gti, root = arguments.gti.resolve(), arguments.root.resolve()
    selected = set(arguments.cases or names)
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    with tempfile.TemporaryDirectory(prefix="gti-llvm-dispatch-") as temporary:
        work = Path(temporary)
        for name, relative in SUCCESS_FILES.items():
            if name in selected:
                execute_case(gti, root, work, name, root / relative, env)
        for name, text in SUCCESS_SOURCES.items():
            if name in selected:
                source = work / f"{name}.gti"
                source.write_text(text, encoding="utf-8")
                execute_case(gti, root, work, name, source, env)
        if "borrowed-dispatch" in selected:
            text = (root / SUCCESS_FILES["static-dispatch"]).read_text(encoding="utf-8")
            if text.count(ALIAS_ANCHOR) != 1:
                raise AssertionError("borrowed-view anchor is not unique")
            borrowed = work / "borrowed-dispatch.gti"
            borrowed.write_text(text.replace(ALIAS_ANCHOR, ALIAS_ANCHOR + """
    {
      Reader& view = value;
      if (view.read(1) != 11) { return 6; }
      if (via_reader(view, 2) != 12) { return 7; }
      Reader& second = view;
      if (via_reader(second, 3) != 13) { return 8; }
    }
    {
      mut Root& root_view = value;
      if (root_view.base_value() != 7) { return 9; }
      root_view.set_base(9);
      if (root_view.base_value() != 9) { return 10; }
      root_view.set_base(7);
    }
    {
      mut Writer& view = value;
      via_writer(view, 23);
    }
    if (!via_truth(value, true)) { return 11; }
"""),
                                encoding="utf-8")
            execute_case(gti, root, work, "borrowed-dispatch", borrowed, env)
        if "borrowed-mut-reference-chain" in selected:
            text = (root / SUCCESS_FILES["static-dispatch"]).read_text(encoding="utf-8")
            if text.count(ALIAS_ANCHOR) != 1:
                raise AssertionError("mutable-view anchor is not unique")
            source = work / "borrowed-mut-reference-chain.gti"
            source.write_text(text.replace(ALIAS_ANCHOR, ALIAS_ANCHOR + """
    {
      mut Writer& view = value;
      { mut Writer& second = view; via_writer(second, 23); }
    }
    if (!via_truth(value, true)) { return 6; }
"""), encoding="utf-8")
            execute_case(gti, root, work, "borrowed-mut-reference-chain", source, env,
                         cpp_vocabulary_limitation=True)
        lifecycle = root / "tests/fixtures/llvm_dispatch_execution_lifecycle.gti"
        if "virtual-failure-cleanup" in selected:
            execute_case(gti, root, work, "virtual-failure-cleanup", lifecycle, env,
                         failure_needle="return 120 / divisor;")
        if "constructor-rollback" in selected:
            text = lifecycle.read_text(encoding="utf-8")
            marker = "// DISPATCH_LIFECYCLE_MAIN\n"
            if text.count(marker) != 1:
                raise AssertionError("lifecycle main marker is not unique")
            rollback = work / "constructor-rollback.gti"
            rollback.write_text(text.split(marker, 1)[0] + ROLLBACK_MAIN, encoding="utf-8")
            execute_case(gti, root, work, "constructor-rollback", rollback, env,
                         failure_needle="quotient(12 / divisor)")
        if "cleanup-oracle-wrong-trace" in selected:
            text = lifecycle.read_text(encoding="utf-8")
            anchor = "mut int expected_cleanup = 1234321;"
            if text.count(anchor) != 1:
                raise AssertionError("cleanup oracle mutation anchor is not unique")
            source = work / "cleanup-oracle-wrong-trace.gti"
            source.write_text(text.replace(anchor, "mut int expected_cleanup = 1234322;"),
                              encoding="utf-8")
            line = next(index for index, value in enumerate(text.splitlines(), 1)
                        if "return 120 / divisor;" in value)
            for endpoint in ENDPOINTS:
                case = work / f"wrong-trace-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard or 'native'}"
                result = execute(gti, root, case, source, endpoint, env)
                if (result.error or result.returncode != 70 or result.stdout
                        or "GTI-R0014" not in result.stderr
                        or "primary [GTI-R0002]" not in result.stderr
                        or f"{source.name}:{line}" not in result.stderr
                        or expected_failure(result, source, line)):
                    raise AssertionError(f"controlled wrong trace failed to discriminate: {endpoint}: {result}")
            print("cleanup-oracle-wrong-trace: secondary failure reproduced; positive oracle rejects the mutation", flush=True)
        for name, text in REJECTIONS.items():
            if name in selected:
                reject_case(gti, root, work, name, text, env)
        for name, text in SOURCE_REJECTIONS.items():
            if name not in selected:
                continue
            case = work / name
            case.mkdir()
            source = case / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for mode in ("O0", "O3"):
                output = case / f"program-{mode}"
                sentinel = b"existing source-rejection artifact\n"
                output.write_bytes(sentinel)
                result = invoke([str(gti), str(source), "--backend", "llvm",
                                 f"-{mode}", "-o", str(output)], root, env)
                if (result.returncode != 65 or "GTI-S" not in result.stderr
                        or "GTI-B0003" in result.stderr
                        or output.read_bytes() != sentinel):
                    raise AssertionError(f"{name} {mode}: {result}")
            print(f"{name}: source rejection and artifact preservation passed", flush=True)


if __name__ == "__main__":
    main()
