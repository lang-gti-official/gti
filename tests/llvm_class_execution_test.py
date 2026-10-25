#!/usr/bin/env python3
"""Concrete class/lifetime execution through independent native endpoints."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


CASES = {
    "ordinary": r'''
mut int events = 0;
class Ticket {
public:
  mut int value = 1;
  Ticket(int input) : value(input) { events = events * 10 + 1; }
  ~Ticket() { events = events * 10 + 2; }
  int read() { return this.value; }
};
int main() {
  { Ticket ticket{7}; if (ticket.read() != 7) { return 1; } }
  return events == 12 ? 0 : 2;
}
''',
    "default-and-generic": r'''
struct Plain { int value = 7; };
class Cell<T> {
  mut T value;
public:
  Cell(T value) : value(value) {}
  T read() { return this.value; }
  void set(T value) mut { this.value = value; }
};
int main() {
  Plain plain{};
  mut Cell<int> cell{5};
  cell.set(9);
  return plain.value == 7 && cell.read() == 9 ? 0 : 1;
}
''',
    "copy-snapshot": r'''
class Cell {
public:
  mut int value;
  Cell(int value) : value(value) {}
};
int inspect(Cell copy, mut Cell& source) { source.value = 99; return copy.value; }
int main() {
  mut Cell original{7};
  Cell snapshot = original;
  original.value = 8;
  if (snapshot.value != 7 || inspect(original, original) != 8) { return 1; }
  return original.value == 99 ? 0 : 2;
}
''',
    "move-owner": r'''
mut int destroyed = 0;
mut int observed = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { destroyed += this.id; }
  int read() { return this.id; }
};
void consume(Ticket value) { observed = value.read(); }
int main() {
  { mut Ticket first{3}; Ticket second = Ticket(std::move(first)); consume(std::move(second)); }
  return destroyed == 3 && observed == 3 ? 0 : 1;
}
''',
    "nested-cleanup": r'''
mut int events = 0;
void record(int value) { events = events * 10 + value; }
class Child {
  int id;
public:
  Child(int id) : id(id) { record(id); }
  ~Child() { record(this.id + 4); }
};
class Parent {
  Child left;
  Child right;
public:
  Parent() : left(Child(1)), right(Child(2)) { record(3); }
  ~Parent() { record(4); }
};
int main() { { Parent value{}; } return events == 123465 ? 0 : 1; }
''',
    "return-and-replace": r'''
mut int destroyed = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { destroyed = destroyed * 10 + this.id; }
  int read() { return this.id; }
};
Ticket make(int id) { Ticket value{id}; return value; }
int main() {
  { mut Ticket value = make(1); value = make(2); if (value.read() != 2) { return 1; } }
  return destroyed == 12 ? 0 : 2;
}
''',
    "class-array": r'''
mut int destroyed = 0;
class Item {
  int id;
public:
  Item(int id) : id(id) {}
  ~Item() { destroyed = destroyed * 10 + this.id; }
  int read() { return this.id; }
};
int main() {
  { Item values[2] = {Item(1), Item(2)}; if (values[0].read() != 1) { return 1; } }
  return destroyed == 21 ? 0 : 2;
}
''',
    "static-storage": r'''
struct Plain { int value = 7; };
Plain global{};
class Counter {
public:
  static mut int value = 3;
  static Plain plain{};
};
int main() { Counter::value += 4; return global.value == 7 && Counter::plain.value == 7 && Counter::value == 7 ? 0 : 1; }
''',
    "tls-passive-dynamic": r'''
mut int initialized = 0;
int initialize() { initialized += 1; return 4; }
thread_local mut int passive = 3;
thread_local bool enabled = true;
thread_local mut int dynamic = initialize();
class State { public: static thread_local mut int count = initialize(); };
int main() {
  if (initialized != 0 || !enabled || passive != 3 || dynamic != 4 || dynamic != 4) { return 1; }
  if (State::count != 4 || State::count != 4) { return 2; }
  return initialized == 2 ? 0 : 3;
}
''',
    "tls-object": r'''
#include <std/panic>
mut int events = 0;
class Item {
  int id;
public:
  Item(int id) : id(id) { events = events * 10 + id; }
  ~Item() {
    if (this.id == 2 && events != 12) { std::panic("bad TLS second cleanup"); }
    if (this.id == 1 && events != 122) { std::panic("bad TLS first cleanup"); }
    events = events * 10 + this.id;
  }
  int read() { return this.id; }
};
thread_local Item first{1};
thread_local Item second{2};
int main() { return first.read() == 1 && second.read() == 2 && first.read() == 1 && events == 12 ? 0 : 1; }
''',
    "borrowed-record": r'''
class View {
  mut int& target;
public:
  View(mut int& target) : target(target) {}
  int read() { return this.target; }
  void set(int value) mut { this.target = value; }
};
int main() {
  mut int value = 3;
  { mut View view{value}; view.set(8); if (view.read() != 8) { return 1; } }
  return value == 8 ? 0 : 2;
}
''',
    "designated-and-overloaded": r'''
struct Point { int x = 1; int y = 2; };
class Cell {
  int value;
public:
  Cell() : value(3) {}
  Cell(int value) : value(value) {}
  int read() { return this.value; }
};
int main() {
  Point point{.x = 4, .y = 5}; Cell first{}; Cell second{6};
  return point.x == 4 && point.y == 5 && first.read() == 3 && second.read() == 6 ? 0 : 1;
}
''',
}

FAILURES = {
    "constructor-rollback": (r'''
#include <std/panic>
mut int events = 0;
class Guard {
public:
  Guard() {}
  ~Guard() { if (events != 21) { std::panic("incorrect constructor rollback"); } }
};
class Field {
  int id;
public:
  Field(int id) : id(id) { if (id == 3) { std::panic("constructor failure"); } }
  ~Field() { events = events * 10 + this.id; }
};
class Outer {
  Field first;
  Field second;
  Field third;
public:
  Outer() : first(Field(1)), second(Field(2)), third(Field(3)) {}
  ~Outer() { std::panic("incomplete object destructor executed"); }
};
int main() { Guard guard{}; Outer value{}; return 0; }
''', "GTI-R0017", "constructor failure"),
    "recursive-tls": (r'''
thread_local int value = read();
int read() { return value; }
int main() { return value; }
''', "GTI-R0013", "recursive_thread_local_initialization"),
    "tls-shutdown": (r'''
int initialize() { return 2; }
thread_local int fresh = initialize();
mut int observed = 0;
class Item { public: Item() {} ~Item() { observed = fresh; } int read() { return 1; } };
thread_local Item late{};
int main() { return late.read() - 1; }
''', "GTI-R0013", "thread_local_initialization_during_shutdown"),
}

NATIVE_ONLY = {
    # Existing C++ vocabulary rejects mutation after a dynamic local TLS
    # initializer. This is retained as an explicit comparator limitation.
    "function-local-tls": r'''
mut int initialized = 0;
int initialize() { initialized += 1; return 4; }
int local() { thread_local mut int count = initialize(); count += 1; return count; }
int main() { return local() == 5 && local() == 6 && initialized == 1 ? 0 : 1; }
''',
    "tls-compound-operators": r'''
int initialize() { return 4; }
int local() {
  thread_local mut int value = initialize();
  value += 3; value *= 2; value -= 1; value /= 3; value %= 4;
  value |= 8; value &= 15; value ^= 2; value <<= 1; value >>= 1;
  return value;
}
int main() { return local() == 10 ? 0 : 1; }
''',
}

NATIVE_FAILURES = {
    "tls-compound-narrow": (r'''
int8_t initialize() { return int8_t(127); }
int local() { thread_local mut int8_t value = initialize(); value += 1; return 0; }
int main() { return local(); }
''', "GTI-R0006"),
    "tls-compound-shift": (r'''
int initialize() { return 4; }
int local(int count) { thread_local mut int value = initialize(); value <<= count; return value; }
int main() { return local(32); }
''', "GTI-R0005"),
}


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    CASES["shipped-example"] = (root / "examples/llvm/classes-and-lifetimes.gti").read_text(encoding="utf-8")
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in CASES and selected not in FAILURES and selected not in NATIVE_ONLY and selected not in NATIVE_FAILURES:
        raise ValueError(f"unknown class execution case: {selected}")
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", mode) for mode in ("O0", "O1", "O2", "O3")))
    with tempfile.TemporaryDirectory(prefix="gti-llvm-classes-") as directory:
        work = Path(directory)
        for name, text in CASES.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints:
                case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}"
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: C++20/23 O3 and LLVM O0-O3 passed", flush=True)
        for name, (text, code, message) in FAILURES.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            observables = []
            for endpoint in endpoints:
                case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}"
                result = execute(gti, root, case, source, endpoint, env)
                if (result.error or result.returncode != 70 or code not in result.stderr
                        or message not in result.stderr or "incorrect" in result.stderr
                        or "incomplete object" in result.stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observables.append(result.observable)
            if len(set(observables)) != 1:
                raise AssertionError(f"failure attribution drift in {name}: {observables}")
            print(f"{name}: exact C++20/23 and LLVM O0-O3 failure passed", flush=True)
        for name, text in NATIVE_ONLY.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints[2:]:
                result = execute(gti, root, work / f"{name}-{endpoint.optimization}", source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: LLVM O0-O3 passed (C++ comparator has a recorded vocabulary limitation)", flush=True)
        for name, (text, code) in NATIVE_FAILURES.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints[2:]:
                result = execute(gti, root, work / f"{name}-{endpoint.optimization}", source, endpoint, env)
                if result.error or result.returncode != 70 or code not in result.stderr:
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: LLVM O0-O3 checked failure passed", flush=True)


if __name__ == "__main__":
    main()
