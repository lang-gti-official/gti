#!/usr/bin/env python3
"""Sweep payload-enum kind x operation.

Owning payload enums landed in 87e1d602 and already produced one defect that
emitted invalid C++ on the old C++ backend (S05). This crosses the payload kinds against the operations
a program performs on them.
"""
import sys

import matrix_support

M = matrix_support.Matrix(__doc__.split("\n\n")[0], 'payload-enum')
OUT = M.out

HEAD = """import std.vector;
import std.string;

mut int32_t drops = 0;
class Res { mut int32_t n = 0; public: Res(int32_t n) : n(n) {} ~Res() { drops = drops + 1; } int32_t get() { return this.n; } };
class Plain { mut int32_t n = 0; public: Plain(int32_t n) : n(n) {} int32_t get() { return this.n; } };
"""

KINDS = {
    "tag-only":     ("enum class E { a, b, };", "E::a", "E::b"),
    "int-payload":  ("enum class E { a(int32_t v), b, };", "E::a(1)", "E::b"),
    "two-field":    ("enum class E { a(int32_t v, int32_t w), b, };", "E::a(1, 2)", "E::b"),
    "plain-class":  ("enum class E { a(Plain p), b, };", "E::a(Plain(1))", "E::b"),
    "owning-dtor":  ("enum class E { a(Res r), b, };", "E::a(Res(1))", "E::b"),
    "owning-string":("enum class E { a(std::string s), b, };", "E::a(std::string(\"x\"))", "E::b"),
}

OPS = {
    "init-and-switch": "mut E e = {A}; total = kind(e);",
    "assign-ctor":     "mut E e = {B}; e = {A}; total = kind(e);",
    "assign-named":    "mut E e = {B}; E other = {A}; e = std::move(other); total = kind(e);",
    "move-init":       "E first = {A}; E second = std::move(first); total = kind(second);",
    "pass-by-ref":     "mut E e = {A}; total = kind(e);",
    "return-from-fn":  "mut E e = produce(); total = kind(e);",
    "vector-push":     "mut std::vector<E> bag = std::vector<E>(); bag.push_back({A}); total = int32_t(bag.size());",
    "reassign-loop":   "mut E e = {B}; for (mut int32_t i = 0; i < 2; i++) {{ e = {A}; }} total = kind(e);",
    "nested-scope":    "{{ mut E e = {A}; total = kind(e); }}",
    "switch-both":     "mut E e = {A}; switch (e) {{ case E::a: total = 1; break; case E::b: total = 2; break; }}",
}

results = {}
for kname, (decl, A, B) in KINDS.items():
    for oname, op in OPS.items():
        src = OUT / f"{kname}__{oname}.gti"
        src.write_text(HEAD + decl + f"""
int32_t kind(E& e) {{ switch (e) {{ case E::a: return 1; case E::b: return 2; }} }}
E produce() {{ return {A}; }}

int main() {{
  mut int32_t total = 0;
  {op.format(A=A, B=B)}
  std::println(total);
  return 0;
}}
""")
        results[(kname, oname)] = M.classify(src)

hdr = f"{'payload kind':<15}" + "".join(f"{o:<17}" for o in OPS)
print(hdr); print("-" * len(hdr))
for k in KINDS:
    print(f"{k:<15}" + "".join(f"{results[(k,o)]:<17}" for o in OPS))

bad = [(k, o, v) for (k, o), v in results.items()
       if matrix_support.hard(v)]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for k, o, v in bad:
    print(f"  {v:<10} {k} / {o}")
sys.exit(M.finish(bad))
