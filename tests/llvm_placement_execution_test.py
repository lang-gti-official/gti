#!/usr/bin/env python3
"""Final-address identity and cleanup; no implicit movement of pinned objects."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


PINNED = r'''
mut int constructed = 0;
mut int destroyed = 0;
class Pinned {
  mut Pinned* constructed_at = nullptr;
  int id;
public:
  Pinned(int id) : id(id) {
    this.constructed_at = &this;
    constructed = constructed * 10 + id;
  }
  Pinned(Pinned&) = delete;
  Pinned(Pinned&&) = delete;
  ~Pinned() {
    if (this.constructed_at != &this) { std::panic("relocated pinned object"); }
    destroyed = destroyed * 10 + this.id;
  }
  bool stable() { return this.constructed_at == &this; }
};
Pinned make(int id) { return Pinned(id); }
Pinned relay(int id) { return make(id); }
'''

CASES = {
    "returns-and-arguments": r'''
int inspect(Pinned& value) { return value.stable() ? 0 : 1; }
Pinned choose(bool first) { if (first) { return relay(1); } return Pinned(2); }
int main() {
  { Pinned a = choose(true); Pinned b = choose(false);
    if (inspect(a) != 0 || !b.stable()) { return 1; } }
  return constructed == 12 && destroyed == 21 ? 0 : 2;
}
''',
    "nested-fields": r'''
class Group {
  Pinned first;
  Pinned second;
public:
  Group() : first(relay(1)), second(Pinned(2)) {}
  Group(Group&) = delete;
  Group(Group&&) = delete;
  bool stable() { return this.first.stable() && this.second.stable(); }
};
Group group() { return Group(); }
int main() {
  { Group value = group(); if (!value.stable()) { return 1; } }
  return constructed == 12 && destroyed == 21 ? 0 : 2;
}
''',
    "nested-arrays": r'''
int main() {
  { Pinned values[2][2] = {{relay(1), Pinned(2)}, {make(3), relay(4)}};
    if (!values[0][0].stable() || !values[0][1].stable() ||
        !values[1][0].stable() || !values[1][1].stable()) { return 1; } }
  return constructed == 1234 && destroyed == 4321 ? 0 : 2;
}
''',
    "direct-immovable-array": r'''
int main() {
  { Pinned values[2] = {Pinned(1), Pinned(2)};
    if (!values[0].stable() || !values[1].stable()) { return 1; } }
  return constructed == 12 && destroyed == 21 ? 0 : 2;
}
''',
    "low-alignment-arrays": r'''
class Small {
  int id;
public:
  Small(int id) : id(id) {}
  Small(Small&) = delete;
  Small(Small&&) = delete;
  ~Small() { destroyed = destroyed * 10 + this.id; }
};
int main() {
  { Small values[2][1] = {{Small(1)}, {Small(2)}}; }
  return destroyed == 21 ? 0 : 1;
}
''',
    "field-arrays": r'''
struct Group {
  Pinned values[2][2] = {{Pinned(1), make(2)}, {relay(3), Pinned(4)}};
  bool stable() { return this.values[0][0].stable() && this.values[1][1].stable(); }
};
int main() {
  { Group value{}; if (!value.stable()) { return 1; } }
  return constructed == 1234 && destroyed == 4321 ? 0 : 2;
}
''',
    "stored-reference-publication": r'''
mut int order = 0;
mut int& select(mut int& value) { order = order * 10 + 1; return value; }
int finish() { order = order * 10 + 2; return 42; }
class View {
  mut int& value;
  int stamp;
public:
  View(mut int& value) : value(select(value)), stamp(finish()) {}
  void update() mut { this.value += this.stamp; }
};
int main() {
  mut int value = 3;
  { mut View view = View(value); view.update(); }
  return value == 45 && order == 12 ? 0 : 1;
}

''',
    "array-copy-and-move": r'''
class Cell { public: mut int value; Cell(int value) : value(value) {} };
int inspect(Cell values[2], mut Cell& original) {
  original.value = 9;
  return values[0].value;
}
int main() {
  mut Cell values[2] = {Cell(3), Cell(4)};
  Cell snapshot[2] = values;
  if (inspect(values, values[0]) != 3 || values[0].value != 9 || snapshot[0].value != 3) { return 1; }
  Cell transferred[2] = std::move(values);
  return transferred[0].value == 9 && transferred[1].value == 4 ? 0 : 2;
}
''',
    "projected-reference-publication": r'''
struct Input { mut int first = 5; mut int second = 7; };
mut int visits = 0;
mut int& selected_field(mut Input& input) { visits += 1; return input.second; }
class View {
  mut int& value;
public:
  View(mut Input& input) : value(selected_field(input)) {}
  void update() mut { this.value = 42; }
};
int main() {
  mut Input input{};
  { mut View view = View(input); view.update(); }
  return input.first == 5 && input.second == 42 && visits == 1 ? 0 : 1;
}
''',
    "shared-reference-publication": r'''
mut int visits = 0;
int& identity(int& input) { visits += 1; return input; }
class View {
  int& value;
public:
  View(int& input) : value(identity(input)) {}
  int read() { return this.value; }
};
int main() {
  int value = 42;
  View first{value}; View second{value};
  return first.read() == 42 && second.read() == 42 && visits == 2 ? 0 : 1;
}
''',
    "default-arrays": r'''
class Element {
  mut Element* original = nullptr;
  int id;
public:
  Element() : id(constructed + 1) { constructed++; this.original = &this; }
  Element(Element&) = delete;
  Element(Element&&) = delete;
  ~Element() {
    if (this.original != &this) { std::panic("relocated default array element"); }
    destroyed = destroyed * 10 + this.id;
  }
  bool stable() { return this.original == &this; }
};
int main() {
  { Element values[2][2] = {};
    if (constructed != 4 || !values[0][0].stable() || !values[1][1].stable()) { return 1; } }
  return destroyed == 4321 ? 0 : 2;
}
''',
    "field-defaults": r'''
struct Record {
  Pinned first = relay(1);
  Pinned second = Pinned(2);
  bool stable() { return this.first.stable() && this.second.stable(); }
};
int main() {
  { Record value{}; if (!value.stable()) { return 1; } }
  return constructed == 12 && destroyed == 21 ? 0 : 2;
}
''',
    "return-cleanup": r'''
class Local { public: ~Local() { destroyed = destroyed * 10 + 4; } };
Pinned prepared() { Local guard{}; return relay(1); }
int main() {
  { Pinned result = prepared();
    if (!result.stable() || destroyed != 4) { return 1; } }
  return destroyed == 41 ? 0 : 2;
}
''',
    "tls": r'''
thread_local Pinned item = relay(1);
int main() { return item.stable() && constructed == 1 ? 0 : 1; }
''',
}

REJECT = {
    "implicit-default-arrays": "int main() { Pinned values[2]; return 0; }",
    "copy": "int main() { Pinned a{1}; Pinned b = a; return 0; }",
    "move": "int main() { mut Pinned a{1}; Pinned b = std::move(a); return 0; }",
    "local-return": "Pinned named() { Pinned a{1}; return a; } int main() { return 0; }",
    "copy-argument": "void consume(Pinned value) {} int main() { Pinned a{1}; consume(a); return 0; }",
    "fresh-value-argument": "void consume(Pinned value) {} int main() { consume(make(1)); return 0; }",
    "conditional-value": "Pinned choose(bool b) { return b ? make(1) : make(2); } int main() { return 0; }",
    "array-element-copy": "int main() { Pinned a{1}; Pinned values[1] = {a}; return 0; }",
    "array-element-move": "int main() { mut Pinned a{1}; Pinned values[1] = {std::move(a)}; return 0; }",
    "array-copy": "int main() { Pinned a[1] = {make(1)}; Pinned b[1] = a; return 0; }",
    "array-move": "int main() { mut Pinned a[1] = {make(1)}; Pinned b[1] = std::move(a); return 0; }",
    "escaping-reference": "class View { int& value; public: View(int& value) : value(value) {} }; View bad() { int local = 1; return View(local); } int main() { return 0; }",
    "overlapping-reference": "mut int& select(mut int& value) { return value; } class View { mut int& value; public: View(mut int& value) : value(select(value)) {} int read() { return this.value; } }; int main() { mut int value = 1; View first{value}; View second{value}; return first.read() + second.read(); }",
}

# The unchanged transitional backend physically relocates these immovable
# subobjects. Do not use that behavior as the native semantic oracle.
COMPARATOR_GAPS = {"nested-fields", "field-defaults", "array-copy-and-move",
                   "stored-reference-publication", "projected-reference-publication",
                   "shared-reference-publication"}


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    cases = {**CASES, "example": None}
    if selected and selected not in cases and selected not in REJECT:
        raise ValueError(f"unknown placement case: {selected}")
    env = {**os.environ, "NO_COLOR": "1"}
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", mode) for mode in ("O0", "O1", "O2", "O3")))
    with tempfile.TemporaryDirectory(prefix="gti-llvm-placement-") as directory:
        work = Path(directory)
        for name, text in cases.items():
            if selected and selected != name:
                continue
            source = work / f"{name}.gti"
            prelude = PINNED
            source.write_text((root / "examples/llvm/immovable-placement.gti").read_text()
                              if text is None else '#include <std/panic>\n' + prelude + text,
                              encoding="utf-8")
            for endpoint in endpoints[2:] if name in COMPARATOR_GAPS else endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            comparators = "LLVM O0-O3 (recorded C++ comparator gap)" if name in COMPARATOR_GAPS else "C++20/23 and LLVM O0-O3"
            print(f"{name}: final addresses/cleanup {comparators} passed", flush=True)
        for name, text in REJECT.items():
            if selected and selected != name:
                continue
            source = work / f"{name}.gti"
            source.write_text('#include <std/panic>\n' + PINNED + text, encoding="utf-8")
            for backend in ("cpp", "llvm"):
                output = work / f"reject-{name}-{backend}"
                output.write_text("preserved", encoding="utf-8")
                result = subprocess.run([str(gti), str(source), "--backend", backend,
                                         "-O3", "-o", str(output)], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "error[GTI-S" not in result.stderr
                        or "GTI-B" in result.stderr or output.read_text() != "preserved"):
                    raise AssertionError(f"{name} {backend}: {result}")
            print(f"{name}: semantic rejection and artifact preservation passed", flush=True)


if __name__ == "__main__":
    main()
