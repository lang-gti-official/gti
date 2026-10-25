#!/usr/bin/env python3
"""Native closed sums: exact alternatives, transport, loans and cleanup."""

from pathlib import Path
import os
import sys
import tempfile

from backend_execution import Endpoint, execute


CASES = {
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
int main() {
  Pair pair = create<Pair>(20, 22);
  Unit empty = create<Unit>();
  int value = 42;
  Reading reading = create<Reading, int&>(value);
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
    "example": None,
}


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in CASES:
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
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", path, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: C++20/23 and LLVM O0-O3 passed", flush=True)


if __name__ == "__main__":
    main()
