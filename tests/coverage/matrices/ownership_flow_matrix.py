#!/usr/bin/env python3
"""Sweep ownership operation x control-flow context.

Moves, borrows and drops interact with branching, and the borrow checker plus
the drop-arming machinery have to agree on every path. This crosses the two.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/flowmx"); OUT.mkdir(exist_ok=True)

PRELUDE = """#include <std/vector>

mut int32_t drops = 0;

class Res {
  mut int32_t n = 0;
public:
  Res(int32_t n) : n(n) {}
  ~Res() { drops = drops + 1; }
  int32_t get() { return this.n; }
};

int32_t consume(Res r) { return r.get(); }
int32_t borrow(Res& r) { return r.get(); }
"""

# {OP} is placed inside each control-flow context.
OPS = {
    "move-out":     "total = total + consume(std::move(owned)); owned = Res(2);",
    "borrow":       "total = total + borrow(owned);",
    "reassign":     "owned = Res(total + 1);",
    "construct":    "mut Res fresh = Res(total); total = total + fresh.get();",
    "push":         "bag.push_back(Res(total)); total = int32_t(bag.size());",
    "nested-scope": "{ mut Res inner = Res(total); total = total + inner.get(); }",
}

CONTEXTS = {
    "straight":      "{OP}",
    "if-then":       "if (flag) {{ {OP} }}",
    "if-else":       "if (flag) {{ {OP} }} else {{ total = 0; }}",
    "both-arms":     "if (flag) {{ {OP} }} else {{ {OP} }}",
    "for-loop":      "for (mut int32_t i = 0; i < 2; i++) {{ {OP} }}",
    "while-loop":    "mut int32_t k = 0; while (k < 2) {{ {OP} k = k + 1; }}",
    "loop-break":    "for (mut int32_t i = 0; i < 3; i++) {{ if (i == 1) {{ break; }} {OP} }}",
    "loop-continue": "for (mut int32_t i = 0; i < 3; i++) {{ if (i == 1) {{ continue; }} {OP} }}",
    "nested-loop":   "for (mut int32_t i = 0; i < 2; i++) {{ for (mut int32_t j = 0; j < 2; j++) {{ {OP} }} }}",
    "switch-arm":    "switch (tag) {{ case Tag::a: {{ {OP} break; }} case Tag::b: break; }}",
}

def classify(path):
    r = subprocess.run([GTI, str(path), "-o", str(path) + ".bin"],
                       capture_output=True, text=True, timeout=180)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        run = subprocess.run([str(path) + ".bin"], capture_output=True, text=True, timeout=60)
        return "ok" if run.returncode == 0 else f"RUN{run.returncode}"
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

results = {}
for oname, op in OPS.items():
    for cname, ctempl in CONTEXTS.items():
        body = ctempl.format(OP=op)
        src = OUT / f"{oname}__{cname}.gti"
        src.write_text(PRELUDE + f"""
enum class Tag {{ a, b }};

int main() {{
  mut Res owned = Res(1);
  mut std::vector<Res> bag = std::vector<Res>();
  bool flag = true;
  Tag tag = Tag::a;
  mut int32_t total = 0;
  {body}
  std::println(total);
  return 0;
}}
""")
        try:
            results[(oname, cname)] = classify(src)
        except subprocess.TimeoutExpired:
            results[(oname, cname)] = "TIMEOUT"

hdr = f"{'operation':<14}" + "".join(f"{c:<15}" for c in CONTEXTS)
print(hdr); print("-" * len(hdr))
for o in OPS:
    print(f"{o:<14}" + "".join(f"{results[(o,c)]:<15}" for c in CONTEXTS))

bad = [(o, c, v) for (o, c), v in results.items()
       if v in ("ICE", "BAD-CPP", "SILENT", "LINK", "TIMEOUT") or v.startswith("RUN")]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for o, c, v in bad:
    print(f"  {v:<10} {o} in {c}")
