#!/usr/bin/env python3
"""Sweep declaration form x parameter shape x qualifier for compiler defects.

The unnamed-parameter ICE family was found by hand-comparing two cells of this
matrix; this generates the whole product so the remaining holes surface at once.
"""
import itertools, pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "build/gti"
OUT = pathlib.Path("/tmp/matrix"); OUT.mkdir(exist_ok=True)

PARAMS = {
    "value-named":     "int32_t v",
    "value-unnamed":   "int32_t",
    "ref-named":       "int32_t& v",
    "ref-unnamed":     "int32_t&",
    "mutref-named":    "mut int32_t& v",
    "mutref-unnamed":  "mut int32_t&",
    "ptr-named":       "int32_t* v",
    "ptr-unnamed":     "int32_t*",
    "default-named":   "int32_t v = 1",
}

def free_def(p):      return f"int32_t f({p}) {{ return 1; }}"
def free_decl(p):     return f"int32_t f({p});"
def externc(p):       return f'extern "C" {{ int32_t f({p}); }}'
def method_def(p):    return f"class C {{ public: C() {{}} int32_t f({p}) {{ return 1; }} }};"
def method_decl(p):   return f"class C {{ public: C() {{}} int32_t f({p}); }};"
def iface(p):         return f"interface I {{ int32_t f({p}); }};"
def ctor(p):          return f"class C {{ public: C({p}) {{}} }};"
def op_eq(p):         return f"class C {{ public: C() {{}} bool operator==({p}) {{ return true; }} }};"
def generic_def(p):   return f"int32_t f<T>({p}) {{ return 1; }}"
def constexpr_def(p): return f"constexpr int32_t f({p}) {{ return 1; }}"
def static_method(p): return f"class C {{ public: C() {{}} static int32_t f({p}) {{ return 1; }} }};"

FORMS = {
    "free-defined": free_def, "free-declared": free_decl, "extern-c": externc,
    "method-defined": method_def, "method-declared": method_decl,
    "interface": iface, "constructor": ctor, "operator==": op_eq,
    "generic-defined": generic_def, "constexpr-defined": constexpr_def,
    "static-method": static_method,
}

ANCHOR = "class Anchor { public: int32_t id() { return 0; } };\n"

def classify(src_path):
    r = subprocess.run([GTI, str(src_path), "-o", str(src_path) + ".bin"],
                       capture_output=True, text=True, timeout=120)
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

rows = []
for form_name, form in FORMS.items():
    for param_name, param in PARAMS.items():
        # generic form needs T in the signature
        p = param.replace("int32_t", "T") if form_name == "generic-defined" else param
        body = form(p)
        src = OUT / f"{form_name}__{param_name}.gti".replace("==", "eq")
        src.write_text(ANCHOR + body + "\nint main() { return 0; }\n")
        try:
            verdict = classify(src)
        except subprocess.TimeoutExpired:
            verdict = "TIMEOUT"
        rows.append((form_name, param_name, verdict))

hdr = f"{'form':<20}" + "".join(f"{p:<17}" for p in PARAMS)
print(hdr); print("-" * len(hdr))
for form_name in FORMS:
    line = f"{form_name:<20}"
    for param_name in PARAMS:
        v = next(v for f, p, v in rows if f == form_name and p == param_name)
        line += f"{v:<17}"
    print(line)

bad = [(f, p, v) for f, p, v in rows if v in ("ICE", "INVALID-CPP", "SILENT", "LINK", "TIMEOUT")]
print(f"\nhard failures: {len(bad)} / {len(rows)}")
for f, p, v in bad:
    print(f"  {v:<12} {f} / {p}")
