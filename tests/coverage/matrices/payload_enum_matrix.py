#!/usr/bin/env python3
"""Sweep payload-enum kind x operation.

Owning payload enums landed in 87e1d602 and already produced one defect that
emits invalid C++ (S05). This crosses the payload kinds against the operations
a program performs on them.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/enummx"); OUT.mkdir(exist_ok=True)

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

def classify(path):
    r = subprocess.run([GTI, str(path), "-o", str(path) + ".bin"],
                       capture_output=True, text=True, timeout=180)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        run = subprocess.run([str(path) + ".bin"], capture_output=True, text=True, timeout=60)
        return "ok" if run.returncode == 0 else f"RUN{run.returncode}"
    if "GTI-B0001" in out: return "ICE"
    if "Undefined symbols" in out: return "LINK"
    if "use of undeclared" in out or ("error:" in out and "error[GTI-" not in out): return "BADCPP"
    if not out.strip(): return "SILENT"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:").replace("GTI-", "") if code else f"exit{r.returncode}"

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
        try:
            results[(kname, oname)] = classify(src)
        except subprocess.TimeoutExpired:
            results[(kname, oname)] = "TIMEOUT"

hdr = f"{'payload kind':<15}" + "".join(f"{o:<17}" for o in OPS)
print(hdr); print("-" * len(hdr))
for k in KINDS:
    print(f"{k:<15}" + "".join(f"{results[(k,o)]:<17}" for o in OPS))

bad = [(k, o, v) for (k, o), v in results.items()
       if v in ("ICE", "BADCPP", "SILENT", "LINK", "TIMEOUT") or v.startswith("RUN")]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for k, o, v in bad:
    print(f"  {v:<10} {k} / {o}")
