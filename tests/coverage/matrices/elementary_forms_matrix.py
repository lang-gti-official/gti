#!/usr/bin/env python3
"""Sweep elementary declaration and statement forms.

`mut int32_t r;` turned out to be an ICE, so the most basic surface in the
language is worth enumerating directly rather than assumed covered.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/basicmx"); OUT.mkdir(exist_ok=True)

CASES = {
    # declarations
    "mut-uninit-int":        "mut int32_t r;",
    "mut-init-int":          "mut int32_t r = 0;",
    "const-uninit-int":      "int32_t r;",
    "const-init-int":        "int32_t r = 0;",
    "mut-uninit-array":      "mut int32_t a[3];",
    "mut-init-array":        "mut int32_t a[3] = {};",
    "mut-uninit-ptr":        "mut int32_t* p;",
    "mut-null-ptr":          "mut int32_t* p = nullptr;",
    "auto-init":             "mut auto r = 1;",
    "shadow":                "mut int32_t r = 1; { mut int32_t r = 2; }",
    "multi-decl":            "mut int32_t a = 1, b = 2;",
    "decl-in-for":           "for (mut int32_t i = 0; i < 1; i++) {}",
    "decl-in-if":            "if (true) { mut int32_t x = 1; }",
    # statements
    "empty-stmt":            ";",
    "empty-block":           "{}",
    "nested-empty-blocks":   "{{{}}}",
    "expr-stmt-literal":     "1;",
    "double-semicolon":      "mut int32_t r = 0;;",
    "return-void-in-int":    "if (false) { return 0; }",
    "unreachable-after-ret": "return 0; mut int32_t r = 1;",
    "break-outside-loop":    "if (true) { }",
    "assign-self":           "mut int32_t r = 1; r = r;",
    "chained-assign":        "mut int32_t a = 0; mut int32_t b = 0; a = b = 1;",
    "compound-assign":       "mut int32_t r = 1; r += 1; r -= 1; r *= 2; r /= 2;",
    "incr-decr":             "mut int32_t r = 1; r++; ++r; r--; --r;",
    "comma-expr":            "mut int32_t r = 1; r = (r, 2);",
    "cast-chain":            "mut int32_t r = int32_t(int64_t(int32_t(1)));",
    "deep-paren-int":        "mut int32_t r = ((((1))));",
    "negative-literal":      "mut int32_t r = -2147483648;",
    "hex-oct-bin":           "mut int32_t r = 0xFF + 0b1010;",
    "char-literal":          "char c = 'a';",
    "string-literal":        "std::string_view s = \"abc\";",
    "bool-ops":              "mut bool b = true && false || !true;",
}

def classify(path):
    r = subprocess.run([GTI, str(path), "-o", str(path) + ".bin"],
                       capture_output=True, text=True, timeout=120)
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

hard = []
for name, body in CASES.items():
    src = OUT / f"{name}.gti"
    src.write_text("#include <std/string>\n"
                   "class Anchor { public: int32_t id() { return 0; } };\n"
                   f"int main() {{ {body} return 0; }}\n")
    try:
        v = classify(src)
    except subprocess.TimeoutExpired:
        v = "TIMEOUT"
    print(f"  {name:<24} {v}")
    if v in ("ICE", "BAD-CPP", "SILENT", "LINK", "TIMEOUT") or v.startswith("RUN"):
        hard.append((name, v))

print(f"\nhard failures: {len(hard)} / {len(CASES)}")
for n, v in hard:
    print(f"  {v:<10} {n}")
