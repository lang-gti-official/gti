#!/usr/bin/env python3
"""Sweep generic shape x argument type x use.

Generics are where the earlier corpus found the most ICEs, and instantiation
interacts with constraints, containers and ownership. This crosses the shape of
the generic declaration against the type it is instantiated with.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/genmx"); OUT.mkdir(exist_ok=True)

HEAD = """import std.vector;
import std.string;

class Plain { mut int32_t n = 0; public: Plain(int32_t n) : n(n) {} int32_t get() { return this.n; } };
class Owning { mut std::vector<int32_t> v; public: Owning(int32_t n) : v(std::vector<int32_t>()) {} int32_t get() { return int32_t(this.v.size()); } };
"""

# {T} is the type argument; {V} is an expression of that type.
ARGS = {
    "int32":   ("int32_t", "7"),
    "float":   ("float", "1.5"),
    "bool":    ("bool", "true"),
    "plain":   ("Plain", "Plain(3)"),
    "owning":  ("Owning", "Owning(3)"),
    "vector":  ("std::vector<int32_t>", "std::vector<int32_t>()"),
    "string":  ("std::string", "std::string(\"a\")"),
}

SHAPES = {
    "identity-fn":   ("T identity<std::movable T>(T v) {{ return v; }}",
                      "mut {T} r = identity<{T}>({V}); total = 1;"),
    "byref-fn":      ("int32_t peek<T>(T& v) {{ return 1; }}",
                      "mut {T} a = {V}; total = peek<{T}>(a);"),
    "container-fn":  ("int32_t count<std::movable T>(std::vector<T>& v) {{ return int32_t(v.size()); }}",
                      "mut std::vector<{T}> bag = std::vector<{T}>(); bag.push_back({V}); total = count<{T}>(bag);"),
    "generic-class": ("class Box<std::movable T> {{ mut T v; public: Box(T v) : v(std::move(v)) {{}} int32_t tag() {{ return 1; }} }};",
                      "mut Box<{T}> b = Box<{T}>({V}); total = b.tag();"),
    "nested-generic":("class Box<std::movable T> {{ mut T v; public: Box(T v) : v(std::move(v)) {{}} int32_t tag() {{ return 1; }} }};",
                      "mut Box<Box<{T}>> b = Box<Box<{T}>>(Box<{T}>({V})); total = b.tag();"),
    "vector-of":     ("", "mut std::vector<{T}> bag = std::vector<{T}>(); bag.push_back({V}); total = int32_t(bag.size());"),
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
    if "error:" in out and "error[GTI-" not in out: return "BAD-CPP"
    if not out.strip(): return "SILENT"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:").replace("GTI-", "") if code else f"exit{r.returncode}"

results = {}
for sname, (decl, use) in SHAPES.items():
    for aname, (T, V) in ARGS.items():
        src = OUT / f"{sname}__{aname}.gti"
        src.write_text(HEAD + decl.format(T=T) + f"""
int main() {{
  mut int32_t total = 0;
  {use.format(T=T, V=V)}
  std::println(total);
  return 0;
}}
""")
        try:
            results[(sname, aname)] = classify(src)
        except subprocess.TimeoutExpired:
            results[(sname, aname)] = "TIMEOUT"

hdr = f"{'shape':<16}" + "".join(f"{a:<11}" for a in ARGS)
print(hdr); print("-" * len(hdr))
for s in SHAPES:
    print(f"{s:<16}" + "".join(f"{results[(s,a)]:<11}" for a in ARGS))

bad = [(s, a, v) for (s, a), v in results.items()
       if v in ("ICE", "BAD-CPP", "SILENT", "LINK", "TIMEOUT") or v.startswith("RUN")]
print(f"\nhard failures: {len(bad)} / {len(results)}")
for s, a, v in bad:
    print(f"  {v:<10} {s} / {a}")
