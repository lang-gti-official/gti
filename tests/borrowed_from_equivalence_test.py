#!/usr/bin/env python3
"""Every former [[borrowed_from]] contract keeps a lifetime spelling.

RFC 0004 replaced the attribute with lifetime syntax, and the attribute was
then removed. Each pair below states one contract with the removed attribute
and with lifetime syntax: the attribute form must be refused by the parser
with the removal diagnostic, and the lifetime form must keep its verdict
(compiling to verified MIR, or the listed rejection).
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

PAIRS = {
    "parameter-supplier": (
        """[[borrowed_from(right)]] int& pick(int& left, int& right) { return right; }
int main() { int a = 1; int b = 2; return pick(a, b) - 2; }
""",
        """int&<r> pick<lifetime r>(int& left, int&<r> right) { return right; }
int main() { int a = 1; int b = 2; return pick(a, b) - 2; }
"""),
    "parameter-supplier-retained": (
        """[[borrowed_from(right)]] int& pick(int& left, int& right) { return right; }
int main() { int a = 1; mut int b = 2; int& r = pick(a, b); b = 3; return r; }
""",
        """int&<r> pick<lifetime r>(int& left, int&<r> right) { return right; }
int main() { int a = 1; mut int b = 2; int& r = pick(a, b); b = 3; return r; }
"""),
    "receiver": (
        """struct Box { int value = 4; Box() = default;
  [[borrowed_from(this)]] int& read() { return this.value; } };
int main() { Box box{}; return box.read() - 4; }
""",
        """struct Box { int value = 4; Box() = default;
  int&<this> read() { return this.value; } };
int main() { Box box{}; return box.read() - 4; }
"""),
    "receiver-operator": (
        """struct Slots { int a = 1; int b = 2; Slots() = default;
  [[borrowed_from(this)]] int& operator[](int i) { if (i == 0) { return this.a; } return this.b; } };
int main() { Slots s{}; return s[1] - 2; }
""",
        """struct Slots { int a = 1; int b = 2; Slots() = default;
  int&<this> operator[](int i) { if (i == 0) { return this.a; } return this.b; } };
int main() { Slots s{}; return s[1] - 2; }
"""),
    "generic-owner-witness": (
        """[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box { int local = 7; Box() = default; int& read() { unsafe { return borrow(&this.local, this); } } };
int main() { Box box{}; return box.read() - 7; }
""",
        """unsafe T&<a> borrow<T, Owner, lifetime a>(const T* address, Owner&<a> owner) { unsafe { return *address; } }
struct Box { int local = 7; Box() = default; int& read() { unsafe { return borrow(&this.local, this); } } };
int main() { Box box{}; return box.read() - 7; }
"""),
    "computed-result-type": (
        """import std.meta;
[[borrowed_from(value)]]
std::meta::callable_return_type<F> borrow<F>(F& fn, int& value) { return fn(value); }
int main() { int owner = 42; auto same = [](int& v) -> int& { return v; };
  int& result = borrow(same, owner); return result - 42; }
""",
        """import std.meta;
std::meta::callable_return_type<F, a> borrow<F, lifetime a>(F& fn, int&<a> value) { return fn(value); }
int main() { int owner = 42; auto same = [](int& v) -> int& { return v; };
  int& result = borrow(same, owner); return result - 42; }
"""),
    "computed-result-wrong-supplier": (
        """import std.meta;
[[borrowed_from(value)]]
std::meta::callable_return_type<F> borrow<F>(F& fn, int& value, int& other) { return fn(other); }
int main() { int owner = 42; int other = 7; auto same = [](int& v) -> int& { return v; };
  int& result = borrow(same, owner, other); return result; }
""",
        """import std.meta;
std::meta::callable_return_type<F, a> borrow<F, lifetime a>(F& fn, int&<a> value, int& other) { return fn(other); }
int main() { int owner = 42; int other = 7; auto same = [](int& v) -> int& { return v; };
  int& result = borrow(same, owner, other); return result; }
"""),
    "protocol-operation": (
        """struct Holder { int value = 5; Holder() = default; };
concept Get<T> { [[borrowed_from(source)]] int& get(T& source); };
for concept Get<Holder> { [[borrowed_from(source)]] int& get(Holder& source) { return source.value; } };
int main() { Holder h{}; int& v = Get<Holder>::get(h); return v - 5; }
""",
        """struct Holder { int value = 5; Holder() = default; };
concept Get<T> { int&<a> get<lifetime a>(T&<a> source); };
for concept Get<Holder> { int&<a> get<lifetime a>(Holder&<a> source) { return source.value; } };
int main() { Holder h{}; int& v = Get<Holder>::get(h); return v - 5; }
"""),
    "requirement-result": (
        """struct Box { int value = 3; Box() = default; int& read() { return this.value; } };
concept readable<C> = requires(C& owner) { { owner.read() } -> int& [[borrowed_from(owner)]]; };
static_assert(readable<Box>);
int main() { return 0; }
""",
        """struct Box { int value = 3; Box() = default; int& read() { return this.value; } };
concept readable<C> = requires(C&<owner> owner) { { owner.read() } -> int&<owner>; };
static_assert(readable<Box>);
int main() { return 0; }
"""),
    "inferred-requirement-result": (
        """struct Box { int value = 3; Box() = default; int& read() { return this.value; } };
concept readable<C> = requires(C& owner) { { owner.read() } -> auto& [[borrowed_from(owner)]]; };
static_assert(readable<Box>);
int main() { return 0; }
""",
        """struct Box { int value = 3; Box() = default; int& read() { return this.value; } };
concept readable<C> = requires(C&<owner> owner) { { owner.read() } -> auto&<owner>; };
static_assert(readable<Box>);
int main() { return 0; }
"""),
    "requirement-carrier-result": (
        """class Window { int& first; public: Window(int& first) : first(first) {} };
struct Src { int value = 1; Src() = default; Window view() { return Window(this.value); } };
concept views<S> = requires(S& source) { { source.view() } -> Window [[borrowed_from(source)]]; };
static_assert(views<Src>);
int main() { Src s{}; Window w = s.view(); return 0; }
""",
        """class Window { int& first; public: Window(int& first) : first(first) {} };
struct Src { int value = 1; Src() = default; Window view() { return Window(this.value); } };
concept views<S> = requires(S&<source> source) { { source.view() } -> Window<source>; };
static_assert(views<Src>);
int main() { Src s{}; Window w = s.view(); return 0; }
"""),
    "inferred-requirement-carrier-result": (
        """class Window { int& first; public: Window(int& first) : first(first) {} };
struct Src { int value = 1; Src() = default; Window view() { return Window(this.value); } };
concept views<S> = requires(S& source) { { source.view() } -> auto [[borrowed_from(source)]]; };
static_assert(views<Src>);
int main() { Src s{}; Window w = s.view(); return 0; }
""",
        """class Window { int& first; public: Window(int& first) : first(first) {} };
struct Src { int value = 1; Src() = default; Window view() { return Window(this.value); } };
concept views<S> = requires(S&<source> source) { { source.view() } -> auto<source>; };
static_assert(views<Src>);
int main() { Src s{}; Window w = s.view(); return 0; }
"""),
}


def normalize(text):
    text = re.sub(r"artifact=[0-9a-f]{64}", "artifact=H", text)
    text = re.sub(r"descriptor-bytes=\d+", "descriptor-bytes=N", text)
    text = re.sub(r"source=\d+:[^ ]+ line=\d+", "source=C:F line=L", text)
    text = re.sub(r"\bspan=\d+\.\.\d+", "span=S", text)
    text = re.sub(r"(unit\d+):\d+@\d+\.\.\d+", r"\1:L@S", text)
    text = re.sub(r"\bline=\d+", "line=L", text)
    text = re.sub(r"\bcolumn=\d+", "column=C", text)
    return re.sub(r"caller-origin=\d+:\d+:\d+:\d+", "caller-origin=P", text)


def compile_one(compiler, stdlib, directory, name, source):
    path = directory / (name + ".gti")
    path.write_text(source)
    mir = directory / (name + ".mir")
    env = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
    result = subprocess.run(
        [str(compiler), str(path), "--emit-mir", "--mir-normalize-loans",
         "--mir-normalize-borrow-dependencies", "-o", str(mir)],
        env=env, capture_output=True, text=True, timeout=300)
    codes = sorted(set(re.findall(r"error\[(GTI-[A-Z]\d{4})\]",
                                  result.stdout + result.stderr)))
    text = normalize(mir.read_text()) if result.returncode == 0 else None
    return result.returncode, codes, text


REJECTED = {"computed-result-wrong-supplier": ["GTI-S2017"],
            "parameter-supplier-retained": ["GTI-S2017"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("stdlib", type=pathlib.Path)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        directory = pathlib.Path(temporary)
        for label, (attribute, lifetime) in PAIRS.items():
            removed = compile_one(args.compiler, args.stdlib, directory,
                                  label + "-attribute", attribute)
            current = compile_one(args.compiler, args.stdlib, directory,
                                  label + "-lifetime", lifetime)
            expected = REJECTED.get(label, [])
            if removed[0] == 0 or removed[1] != ["GTI-P0001"]:
                failures.append(f"{label}: attribute form was not refused "
                                f"(exit {removed[0]} {removed[1]})")
            elif (current[0] == 0) != (not expected) or current[1] != expected:
                failures.append(f"{label}: lifetime form exit {current[0]} "
                                f"{current[1]}, expected {expected or 'success'}")
            else:
                print(f"PASS {label}")
    if failures:
        print("\n".join("FAIL " + failure for failure in failures))
        sys.exit(1)


if __name__ == "__main__":
    main()
