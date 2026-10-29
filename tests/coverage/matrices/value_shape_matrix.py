#!/usr/bin/env python3
"""Sweep value-producer x consumer for emitter and analysis holes.

Several confirmed ICEs (method on an operator result, a view borrowed from a
returned owner, a span returned by plain copy) are all "temporary produced one
way, consumed another". This generates that product directly.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "build/gti"
OUT = pathlib.Path("/tmp/tempmx"); OUT.mkdir(exist_ok=True)

PRELUDE = """import std.vector;
import std.string;

class Val {
  mut int32_t n = 0;
public:
  Val(int32_t n) : n(n) {}
  int32_t get() { return this.n; }
  Val twin() { return Val(this.n); }
  mut int32_t& slot() mut { return this.n; }
  Val operator+(Val& o) { return Val(this.n + o.n); }
};

Val make() { return Val(3); }
Val identity(Val v) { return v; }
int32_t consume_value(Val v) { return v.get(); }
int32_t consume_ref(Val& v) { return v.get(); }
"""

# Each producer is an expression of type Val.
PRODUCERS = {
    "constructor":    "Val(1)",
    "free-call":      "make()",
    "method-call":    "named.twin()",
    "operator":       "(named + named)",
    "passthrough":    "identity(Val(1))",
    "chained-method": "named.twin().twin()",
    "moved-local":    "std::move(named)",
    "ternary":        "(flag ? Val(1) : Val(2))",
}

# Each consumer takes the producer expression P.
CONSUMERS = {
    "init-local":       "mut Val x = {P}; total = x.get();",
    "method-on-temp":   "total = {P}.get();",
    "pass-by-value":    "total = consume_value({P});",
    "return-from-fn":   "total = consume_value(identity({P}));",
    "store-in-vector":  "bag.push_back({P}); total = int32_t(bag.size());",
    "operator-on-temp": "mut Val y = Val(0); total = ({P} + y).get();",
    "chain-twin":       "total = {P}.twin().get();",
    "discarded":        "[[discard]] {P}.get(); total = 1;",
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
        return "INVALID-CPP"
    if not out.strip():
        return "SILENT"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:") if code else f"exit{r.returncode}"

results = {}
for pname, pexpr in PRODUCERS.items():
    for cname, ctempl in CONSUMERS.items():
        body = ctempl.format(P=pexpr)
        src = OUT / f"{pname}__{cname}.gti"
        src.write_text(PRELUDE + f"""
int main() {{
  mut Val named = Val(5);
  mut std::vector<Val> bag = std::vector<Val>();
  bool flag = true;
  mut int32_t total = 0;
  {body}
  std::println(total);
  return 0;
}}
""")
        try:
            results[(pname, cname)] = classify(src)
        except subprocess.TimeoutExpired:
            results[(pname, cname)] = "TIMEOUT"

hdr = f"{'producer':<16}" + "".join(f"{c:<18}" for c in CONSUMERS)
print(hdr); print("-" * len(hdr))
for p in PRODUCERS:
    print(f"{p:<16}" + "".join(f"{results[(p,c)]:<18}" for c in CONSUMERS))

bad = [(p, c, v) for (p, c), v in results.items()
       if v in ("ICE", "INVALID-CPP", "SILENT", "LINK", "TIMEOUT")]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for p, c, v in bad:
    print(f"  {v:<12} {p} -> {c}")
