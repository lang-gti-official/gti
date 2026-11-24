#!/usr/bin/env python3
"""Sweep class kind x operation.

S05 showed a defect that needed a payload type with a destructor. This crosses
the kinds of class a program actually declares against the operations done to
them, since cleanup obligations change which emitter path is taken.
"""
import sys

import matrix_support

M = matrix_support.Matrix(__doc__.split("\n\n")[0], 'class-kind')
OUT = M.out

HEAD = """import std.vector;
import std.string;
import std.memory;

mut int32_t drops = 0;
"""

KINDS = {
    "plain":      "class T { mut int32_t n = 0; public: T(int32_t n) : n(n) {} int32_t get() { return this.n; } };",
    "with-dtor":  "class T { mut int32_t n = 0; public: T(int32_t n) : n(n) {} ~T() { drops = drops + 1; } int32_t get() { return this.n; } };",
    "owns-string":"class T { mut std::string s; public: T(int32_t n) : s(std::string(\"x\")) {} int32_t get() { return int32_t(this.s.size()); } };",
    "owns-vector":"class T { mut std::vector<int32_t> v; public: T(int32_t n) : v(std::vector<int32_t>()) {} int32_t get() { return int32_t(this.v.size()); } };",
    "owns-unique":"class T { mut std::unique_ptr<int32_t> p; public: T(int32_t n) : p(std::make_unique<int32_t>(n)) {} int32_t get() { return *this.p; } };",
    "derived":    "class Base { public: Base() {} virtual int32_t k() { return 1; } };\nclass T : public Base { mut int32_t n = 0; public: T(int32_t n) : n(n) {} int32_t k() override { return 2; } int32_t get() { return this.n; } };",
}

OPS = {
    "construct-read":  "mut T a = T(1); total = a.get();",
    "move-local":      "mut T a = T(1); mut T b = std::move(a); total = b.get();",
    "pass-by-value":   "mut T a = T(1); total = sink(std::move(a));",
    "pass-by-ref":     "mut T a = T(1); total = peek(a);",
    "return-from-fn":  "mut T a = produce(); total = a.get();",
    "vector-push":     "mut std::vector<T> bag = std::vector<T>(); bag.push_back(T(1)); total = int32_t(bag.size());",
    "vector-iterate":  "mut std::vector<T> bag = std::vector<T>(); bag.push_back(T(1)); for (auto& e : bag) { total = total + e.get(); }",
    "unique-box":      "mut auto p = std::make_unique<T>(1); total = p->get();",
    "reassign":        "mut T a = T(1); a = T(2); total = a.get();",
    "nested-scope":    "{ mut T a = T(1); total = a.get(); }",
}

SUPPORT = """
int32_t sink(T v) { return v.get(); }
int32_t peek(T& v) { return v.get(); }
T produce() { return T(3); }
"""

results = {}
for kname, kdecl in KINDS.items():
    for oname, op in OPS.items():
        src = OUT / f"{kname}__{oname}.gti"
        src.write_text(HEAD + kdecl + SUPPORT + f"""
int main() {{
  mut int32_t total = 0;
  {op}
  std::println(total);
  return 0;
}}
""")
        results[(kname, oname)] = M.classify(src)

hdr = f"{'kind':<13}" + "".join(f"{o:<16}" for o in OPS)
print(hdr); print("-" * len(hdr))
for k in KINDS:
    print(f"{k:<13}" + "".join(f"{results[(k,o)]:<16}" for o in OPS))

bad = [(k, o, v) for (k, o), v in results.items()
       if matrix_support.hard(v)]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for k, o, v in bad:
    print(f"  {v:<10} {k} / {o}")
sys.exit(M.finish(bad))
