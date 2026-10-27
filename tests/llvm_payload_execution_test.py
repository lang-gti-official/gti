#!/usr/bin/env python3
"""Native closed sums: exact alternatives, transport, loans and cleanup."""

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


RESOURCE = r'''
#include <std/panic>
mut int trace = 0;
mut int expected_trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
};
enum Value { empty, one(Resource value), two(Resource first, Resource second) };
struct Guard {
  ~Guard() {
    if (trace != expected_trace) { std::panic("incorrect payload cleanup"); }
  }
};
'''

FAILURES = {
    "payload-destructor-failure": (RESOURCE + r'''
class Dying {
  int id;
public:
  Dying(int id) : id(id) {}
  Dying(Dying&) = delete;
  Dying(Dying&&) = default;
  ~Dying() {
    trace = trace * 10 + this.id;
    if (this.id == 2) { std::panic("payload drop failure"); }
  }
};
enum Failing { pair(Dying first, Dying second) };
int main() {
  expected_trace = 21;
  Guard guard{};
  { Failing value = Failing::pair(Dying(1), Dying(2)); }
  return 0;
}
''', "GTI-R0017", "payload drop failure"),
    "staged-payload-failure": (RESOURCE + r'''
Resource checked() { std::panic("payload staging failure"); }
int main() {
  expected_trace = 1;
  Guard guard{};
  Value value = Value::two(Resource(1), checked());
  return 0;
}
''', "GTI-R0017", "payload staging failure"),
    "prepared-enum-input-failure": (RESOURCE + r'''
int checked() { std::panic("prepared enum failure"); }
void consume(Value value, int tail) {}
int main() {
  expected_trace = 21;
  Guard guard{};
  consume(Value::two(Resource(1), Resource(2)), checked());
  return 0;
}
''', "GTI-R0017", "prepared enum failure"),
    "empty-optional": (RESOURCE + r'''
#include <std/optional>
int main() {
  Guard guard{};
  std::optional<int> value{};
  [[discard]] value.value();
  return 0;
}
''', "GTI-R0008", "empty_owner_access"),
    "inactive-value": (RESOURCE + r'''
int main() {
  Guard guard{};
  expected<int, int> value = unexpected(7);
  [[discard]] value.value();
  return 0;
}
''', "GTI-R0009", "invalid_expected_access"),
    "inactive-error": (RESOURCE + r'''
int main() {
  Guard guard{};
  expected<int, int> value = 42;
  [[discard]] value.error();
  return 0;
}
''', "GTI-R0009", "invalid_expected_access"),
}

REJECT = {
    "union-safe-access": "union U { mut int value; }; int main() { U value{}; return value.value; }",
    "nonexhaustive": "enum E { empty, item(int value) }; int main() { E value = E::empty; switch(value) { case E::empty: return 0; } }",
    "escaped-payload": "enum E { item(int value) }; int& bad() { E owner = E::item(1); switch(owner) { case E::item(auto& value): return value; } } int main() { return 0; }",
    "read-only-payload": "enum E { item(int value) }; int main() { E owner = E::item(1); switch(owner) { case E::item(mut auto& value): value = 2; break; } return 0; }",
    "moved-subject": "enum E { item(int value) }; int main() { mut E owner = E::item(1); switch(std::move(owner)) { case E::item(value): break; } switch(owner) { case E::item(value): return value; } }",
}

REJECTION_MESSAGES = {
    "union-safe-access": ("union member access", "requires an unsafe block"),
    "nonexhaustive": ("GTI-S2067", "not exhaustive"),
    "escaped-payload": ("return", "reference"),
    "read-only-payload": ("mutable payload pattern", "mutable enum receiver"),
    "moved-subject": ("has already been moved",),
}

CASES = {
    "native-borrow-group": r'''
class View {
  int& first;
  int& second;
public:
  View(int& first, int& second) : first(first), second(second) {}
  int& select(bool first) {
    if (first) { return this.first; }
    return this.second;
  }
};
int main() {
  int first = 20;
  int second = 22;
  View view{first, second};
  return &view.select(true) == &first && &view.select(false) == &second ? 0 : 1;
}
''',
    "constant-storage": r'''
enum Data { empty, item(int value), pair(uint8_t small, uint64_t large), flag(bool value) };
constexpr Data create() { return Data::pair(uint8_t(3), uint64_t(39)); }
constexpr Data empty() { return Data::empty; }
constexpr Data flag() { return Data::flag(true); }
constexpr Data first = create();
constexpr Data none = empty();
constexpr Data boolean = flag();
constexpr Data values[2] = {create(), empty()};
enum Text { empty, data(std::string_view bytes) };
constexpr Text text() { return Text::data("A\0B"); }
constexpr Text literal = text();
struct Holder {
  Data value;
  constexpr Holder(Data value) : value(value) {}
};
constexpr Holder holder = Holder(create());
int read(Data& data) {
  switch (data) {
  case Data::empty: return 0;
  case Data::item(auto& value): return value;
  case Data::pair(auto& small, auto& large): return int(small) + int(large);
  case Data::flag(auto& value): return value ? 42 : 0;
  }
}
int read_text(Text& text) {
  switch (text) {
  case Text::empty: return 2;
  case Text::data(auto& bytes):
    if (bytes.size() != uint64_t(3) || bytes[0] != 'A' || bytes[1] != '\0' || bytes[2] != 'B') { return 3; }
    break;
  }
  return 0;
}
int main() {
  if (read_text(literal) != 0) { return 4; }
  return read(first) == 42 && read(none) == 0 && read(values[0]) == 42 &&
         read(values[1]) == 0 && read(holder.value) == 42 && read(boolean) == 42 ? 0 : 1;
}
''',
    "program-storage": r'''
enum Data { empty, item(int value) };
union Overlay { mut uint64_t bits; mut double value; };
mut Overlay overlay{};
Data fixed = Data::item(42);
Data global = Data::item(43);
thread_local Data local = Data::item(44);
int read(Data& data) {
  switch (data) {
  case Data::empty: return 0;
  case Data::item(auto& value): return value;
  }
}
int main() {
  unsafe {
    if (overlay.bits != uint64_t(0)) { return 2; }
    overlay.value = 1.0;
    if (overlay.bits != uint64_t(4607182418800017408)) { return 3; }
  }
  return read(fixed) == 42 && read(global) == 43 && read(local) == 44 ? 0 : 1;
}
''',
    "nested-payload": r'''
mut int trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
  int read() { return this.id; }
};
enum Inner { empty, item(Resource value) };
enum Outer { empty, group(Inner value, int coordinates[2]) };
int main() {
  {
    Outer owner = Outer::group(Inner::item(Resource(1)), {20, 22});
    switch (owner) {
    case Outer::empty: return 1;
    case Outer::group(auto& inner, auto& coordinates):
      if (coordinates[0] + coordinates[1] != 42) { return 2; }
      switch (inner) {
      case Inner::empty: return 3;
      case Inner::item(auto& value): if (value.read() != 1) { return 4; } break;
      }
      break;
    }
  }
  return trace == 1 ? 0 : 5;
}

''',
    "conditional-publication": r'''
mut int trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
  int read() { return this.id; }
};
enum Data { empty, item(Resource value) };
Data create(bool first) { return first ? Data::item(Resource(1)) : Data::item(Resource(2)); }
int read(Data& data) {
  switch (data) {
  case Data::empty: return 0;
  case Data::item(auto& value): return value.read();
  }
}
int main() {
  {
    Data first = create(true);
    Data second = create(false);
    if (read(first) != 1 || read(second) != 2) { return 1; }
  }
  return trace == 21 ? 0 : 2;
}
''',
    "owned-results": r'''
mut int trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
  int read() { return this.id; }
};
expected<Resource, Resource> create(bool ready) {
  if (ready) { return Resource(1); }
  return unexpected(Resource(2));
}
int main() {
  {
    auto first = create(true);
    auto second = create(false);
    if (!first || first.value().read() != 1 || second || second.error().read() != 2) { return 1; }
  }
  return trace == 21 ? 0 : 2;
}
''',
    "stored-reference": r'''
enum View { missing, value(int& payload) };
View wrap(int& value) { return View::value(value); }
expected<int&, int> lookup(int& value, bool ready) {
  if (!ready) { return unexpected(7); }
  return value;
}
expected<int, int> read(int& value, bool ready) {
  int& selected = ?lookup(value, ready);
  return selected;
}
int main() {
  int value = 42;
  View view = wrap(value);
  switch (view) {
  case View::missing: return 1;
  case View::value(auto& selected): if (&selected != &value) { return 2; } break;
  }
  auto success = read(value, true);
  auto failure = read(value, false);
  return success && success.value() == 42 && !failure && failure.error() == 7 ? 0 : 3;
}
''',
    "concrete-packs": r'''
struct Pair {
  int left;
  int right;
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};
struct Unit { Unit() {} };
struct Reading {
  int value;
  Reading(int& value) : value(value) {}
  int read() { return this.value; }
};
T create<T, Args...>(Args... args) { return T(args...); }
int prefixed<Args...>(int prefix, Args... args) {
  Pair pair = create<Pair>(args...);
  return prefix + pair.sum();
}
int main() {
  Pair pair = create<Pair>(20, 22);
  Unit empty = create<Unit>();
  int value = 42;
  Reading reading = create<Reading, int&>(value);
  if (prefixed<int, int>(2, 20, 20) != 42) { return 4; }
  if (create<int>(42) != 42) { return 5; }
  return pair.sum() == 42 && reading.read() == 42 ? 0 : 1;
}
''',
    "generated-copy": r'''
class Counted {
  mut int value;
public:
  Counted(int value) : value(value) {}
  Counted(Counted& other) = default;
  Counted(Counted&& other) = default;
  int read() { return this.value; }
  void set(int value) mut { this.value = value; }
};
enum Data { empty, item(Counted value) };
int main() {
  Data data = Data::item(Counted(42));
  mut Data copied = data;
  switch (copied) {
  case Data::empty: return 1;
  case Data::item(mut auto& value): value.set(43); break;
  }
  switch (data) {
  case Data::empty: return 1;
  case Data::item(value):
    if (value.read() != 42) { return 2; }
    break;
  }
  return 0;
}
''',
    "native-union": r'''
union Overlay { mut uint64_t bits; mut double value; };
int main() {
  mut Overlay data{};
  unsafe {
    data.value = 1.0;
    if (data.bits != uint64_t(4607182418800017408)) { return 1; }
    data.bits = uint64_t(0);
    if (data.value != 0.0) { return 2; }
  }
  return 0;
}
''',
    "scalar": r'''
enum Data<T> { empty, number(T value), pair(T left, T right), flag(bool value) };
int read(Data<int> value) {
  switch (value) {
  case Data<int>::empty: return 0;
  case Data<int>::number(number): return number;
  case Data<int>::pair(left, right): return left + right;
  case Data<int>::flag(value): return value ? 42 : 0;
  }
}
int main() {
  Data<int> input = Data<int>::pair(20, 22);
  if (read(input) != 42 || read(Data<int>::empty) != 0 ||
      read(Data<int>::number(42)) != 42 || read(Data<int>::flag(true)) != 42) { return 1; }
  return 0;
}
''',
    "active-cleanup": r'''
mut int trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
  int read() { return this.id; }
};
enum Value { empty, one(Resource value), two(Resource first, Resource second) };
int main() {
  {
    mut Value value = Value::one(Resource(1));
    value = Value::empty;
    value = Value::two(Resource(2), Resource(3));
    switch (std::move(value)) {
    case Value::empty: return 1;
    case Value::one(item): return 2;
    case Value::two(first, second):
      if (first.read() != 2 || second.read() != 3) { return 3; }
      break;
    }
  }
  return trace == 132 ? 0 : 4;
}
''',
    "borrowed-array": r'''
enum Value { empty, numbers(int values[2]) };
int main() {
  mut Value value = Value::numbers({20, 21});
  switch (value) {
  case Value::empty: return 1;
  case Value::numbers(mut auto& numbers): numbers[1] += 1; break;
  }
  switch (value) {
  case Value::empty: return 2;
  case Value::numbers(auto& numbers): return numbers[0] + numbers[1] == 42 ? 0 : 3;
  }
}
''',
    "source-wrappers": r'''
#include <std/optional>
expected<int, int> load(bool ready) {
  if (ready) { return 40; }
  return unexpected(7);
}
expected<int, int> calculate(bool ready) {
  int value = ?load(ready);
  return value + 2;
}
int main() {
  mut std::optional<int> value = std::optional<int>();
  if (value.has_value()) { return 1; }
  value.emplace(42);
  if (!value.has_value() || value.value() != 42) { return 2; }
  value.reset();
  if (value.has_value()) { return 3; }
  auto success = calculate(true);
  auto failure = calculate(false);
  if (!success || success.value() != 42 || failure || failure.error() != 7) { return 4; }
  return 0;
}
''',
    "checked-arithmetic": r'''
#include <std/numeric>
int main() {
  auto add_success = std::checked_add(int8_t(20), int8_t(22));
  auto add_overflow = std::checked_add(int8_t(127), int8_t(1));
  auto sub_success = std::checked_sub(int8_t(44), int8_t(2));
  auto sub_overflow = std::checked_sub(int8_t(-128), int8_t(1));
  auto mul_success = std::checked_mul(int8_t(6), int8_t(7));
  auto mul_overflow = std::checked_mul(uint8_t(200), uint8_t(2));
  if (add_success.value_or(int8_t(0)) != int8_t(42) ||
      add_overflow.has_value() ||
      sub_success.value_or(int8_t(0)) != int8_t(42) ||
      sub_overflow.has_value() ||
      mul_success.value_or(int8_t(0)) != int8_t(42) ||
      mul_overflow.has_value()) {
    return 1;
  }
  return 0;
}
''',
    "payload-raw-access": r'''
#include <std/memory>
#include <std/vector>
mut int trace = 0;
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = default;
  ~Resource() { trace = trace * 10 + this.id; }
  int read() { return this.id; }
};
enum Value { empty, one(Resource value), number(int count) };
int score(Value& value) {
  switch (value) {
  case Value::empty: return 0;
  case Value::one(auto& item): return item.read();
  case Value::number(count): return count;
  }
}
enum Plain { none, pair(int first, int second) };
int sum(Plain& value) {
  switch (value) {
  case Plain::none: return 0;
  case Plain::pair(first, second): return first + second;
  }
}
struct Window {
  mut Plain values[3] = {Plain::pair(1, 2), Plain::none, Plain::pair(3, 5)};
};
int main() {
  {
    // Library slots advance raw payload pointers to append, relocate on
    // growth, read, write and destroy the elements of a payload enum.
    mut std::vector<Value> values{};
    values.push_back(Value::number(4));
    values.push_back(Value::one(Resource(1)));
    values.push_back(Value::empty);
    values.push_back(Value::number(9));
    values.push_back(Value::one(Resource(2)));
    mut int total = 0;
    for (auto& value : values) { total = total * 10 + score(value); }
    if (total != 41092 || values.size() != uint64_t(5)) { return 1; }
    values[uint64_t(2)] = Value::number(7);
    if (score(values[uint64_t(2)]) != 7 || score(values.at(uint64_t(1))) != 1) { return 2; }
    values.pop_back();
    if (trace != 2 || values.size() != uint64_t(4)) { return 3; }
  }
  if (trace != 21) { return 4; }
  // Direct add/subtract in either operand order with signed or unsigned
  // offsets, a staged advanced argument, and checked raw references.
  mut Window window{};
  unsafe {
    Plain* first = &window.values[0];
    Plain* last = uint64_t(2) + first;
    Plain* middle = last - int32_t(1);
    if (sum(std::assume_ref(last, window)) != 8 || sum(std::assume_ref(middle, window)) != 0 ||
        sum(std::assume_ref(first + uint64_t(2), window)) != 8) {
      return 5;
    }
    std::assume_mut(last, window) = Plain::pair(4, 2);
  }
  return sum(window.values[2]) == 6 ? 0 : 6;
}
''',
    "example": None,
}

# This source passes semantics/MIR but the unchanged C++ representation gate
# rejects its conditional owning-result body. Keep the native value/drop oracle.
COMPARATOR_GAPS = {"conditional-publication"}


def evidence_cases(root: Path) -> dict[str, str]:
    cases = {
        name: ((root / "examples/llvm/payloads-and-unions.gti").read_text(
            encoding="utf-8") if source is None else source)
        for name, source in CASES.items()
    }
    cases.update({name: source for name, (source, _, _) in FAILURES.items()})
    return cases


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return {
        "payload-core": tuple(CASES) + tuple(FAILURES),
        "payload-packs": ("concrete-packs",),
        "payload-access-failures": (
            "empty-optional", "inactive-value", "inactive-error"),
        "checked-arithmetic": ("checked-arithmetic",),
        "payload-raw-access": ("payload-raw-access",),
    }


def evidence_manifest(root: Path) -> dict:
    return build_manifest(
        "llvm_payload_execution", evidence_cases(root), evidence_profiles(root),
        {"example": "examples/llvm/payloads-and-unions.gti"})


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()),
                         sort_keys=True))
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in CASES and selected not in FAILURES and selected not in REJECT:
        raise ValueError(f"unknown payload case: {selected}")
    env = {**os.environ, "NO_COLOR": "1"}
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", mode) for mode in ("O0", "O1", "O2", "O3")))
    with tempfile.TemporaryDirectory(prefix="gti-llvm-payloads-") as directory:
        work = Path(directory)
        for name, source in CASES.items():
            if selected and name != selected:
                continue
            path = work / f"{name}.gti"
            path.write_text((root / "examples/llvm/payloads-and-unions.gti").read_text()
                            if source is None else source, encoding="utf-8")
            for endpoint in endpoints[2:] if name in COMPARATOR_GAPS else endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", path, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            scope = "LLVM O0-O3 (recorded C++ vocabulary gap)" if name in COMPARATOR_GAPS else "C++20/23 and LLVM O0-O3"
            print(f"{name}: {scope} passed", flush=True)
        for name, (source, code, message) in FAILURES.items():
            if selected and name != selected:
                continue
            path = work / f"{name}.gti"
            path.write_text(source, encoding="utf-8")
            observed = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", path, endpoint, env)
                if (result.error or result.returncode != 70 or code not in result.stderr
                        or message not in result.stderr or "incorrect payload cleanup" in result.stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"failure attribution differs in {name}: {observed}")
            print(f"{name}: exact failure attribution and cleanup passed", flush=True)
        for name, source in REJECT.items():
            if selected and name != selected:
                continue
            path = work / f"{name}.gti"
            path.write_text(source, encoding="utf-8")
            for backend in ("cpp", "llvm"):
                output = work / f"reject-{name}-{backend}"
                output.write_text("preserved", encoding="utf-8")
                result = subprocess.run([str(gti), str(path), "--backend", backend,
                                         "-O3", "-o", str(output)], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "error[GTI-S" not in result.stderr
                        or "GTI-B" in result.stderr or output.read_text() != "preserved"
                        or any(message.casefold() not in result.stderr.casefold()
                               for message in REJECTION_MESSAGES[name])):
                    raise AssertionError(f"{name} {backend}: {result}")
            print(f"{name}: source rejection and artifact preservation passed", flush=True)


if __name__ == "__main__":
    main()
