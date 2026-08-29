#!/usr/bin/env python3
"""Sweep wrapper x context for class-typed expressions.

S01 showed that a redundant pair of parentheses around a constructor call is
fatal. This asks the general question: which syntactic wrappers, in which
positions, break a class value?
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/wrapmx"); OUT.mkdir(exist_ok=True)

PRELUDE = """#include <std/vector>

class Val {
  mut int32_t n = 0;
public:
  Val(int32_t n) : n(n) {}
  int32_t get() { return this.n; }
  Val twin() { return Val(this.n); }
  Val operator+(Val& o) { return Val(this.n + o.n); }
};

int32_t take(Val v) { return v.get(); }
Val give(Val v) { return v; }

class Holder {
  mut Val inner;
public:
  Holder(Val v) : inner(std::move(v)) {}
  int32_t get() { return this.inner.get(); }
};
"""

# E is the inner class-valued expression.
WRAPPERS = {
    "bare":        "{E}",
    "paren":       "({E})",
    "paren2":      "(({E}))",
    "move":        "std::move({E})",
    "paren-move":  "(std::move({E}))",
}

CONTEXTS = {
    "init-local":     "mut Val x = {W}; total = x.get();",
    "call-argument":  "total = take({W});",
    "nested-call":    "total = take(give({W}));",
    "ctor-argument":  "mut Holder h = Holder({W}); total = h.get();",
    "vector-push":    "bag.push_back({W}); total = int32_t(bag.size());",
    "method-on-it":   "total = {W}.get();",
    "return-value":   "total = give({W}).get();",
}

INNER = {
    "constructor": "Val(1)",
    "method-call": "named.twin()",
}

def classify(path):
    r = subprocess.run([GTI, str(path), "-o", str(path) + ".bin"],
                       capture_output=True, text=True, timeout=180)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        return "ok"
    if "GTI-B0001" in out:
        return "ICE"
    if "Undefined symbols" in out:
        return "LINK"
    if "error:" in out and "error[GTI-" not in out:
        return "BAD-CPP"
    if not out.strip():
        return "SILENT"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:").replace("GTI-", "") if code else f"exit{r.returncode}"

for iname, iexpr in INNER.items():
    print(f"\n=== inner expression: {iexpr} ===")
    results = {}
    for wname, wtempl in WRAPPERS.items():
        for cname, ctempl in CONTEXTS.items():
            wrapped = wtempl.format(E=iexpr)
            body = ctempl.format(W=wrapped)
            src = OUT / f"{iname}__{wname}__{cname}.gti"
            src.write_text(PRELUDE + f"""
int main() {{
  mut Val named = Val(5);
  mut std::vector<Val> bag = std::vector<Val>();
  mut int32_t total = 0;
  {body}
  std::println(total);
  return 0;
}}
""")
            try:
                results[(wname, cname)] = classify(src)
            except subprocess.TimeoutExpired:
                results[(wname, cname)] = "TIMEOUT"
    hdr = f"{'wrapper':<13}" + "".join(f"{c:<16}" for c in CONTEXTS)
    print(hdr); print("-" * len(hdr))
    for w in WRAPPERS:
        print(f"{w:<13}" + "".join(f"{results[(w,c)]:<16}" for c in CONTEXTS))
