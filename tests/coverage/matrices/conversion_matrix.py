#!/usr/bin/env python3
"""Sweep implicit conversion source type x target type.

GTI permits bounded conversions in assignment contexts, while calls match
exact argument types. This records the accepted assignment relation; integer
to float/double may round (S12 is an audited contract, not an open defect).
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/convmx"); OUT.mkdir(exist_ok=True)

TYPES = ["int8_t", "int16_t", "int32_t", "int64_t",
         "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double"]

def classify(path):
    r = subprocess.run([GTI, str(path), "-o", str(path) + ".bin"],
                       capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        return "ok"
    if "GTI-B0001" in out: return "ICE"
    if "error:" in out and "error[GTI-" not in out: return "BADCPP"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:").replace("GTI-S", "S") if code else "?"

results = {}
for src_t in TYPES:
    for dst_t in TYPES:
        p = OUT / f"{src_t}__{dst_t}.gti"
        p.write_text(
            "class Anchor { public: int32_t id() { return 0; } };\n"
            f"int main() {{ mut {src_t} a = {src_t}(1); mut {dst_t} b = a; return 0; }}\n")
        try:
            results[(src_t, dst_t)] = classify(p)
        except subprocess.TimeoutExpired:
            results[(src_t, dst_t)] = "TIMEOUT"

print("rows = source type, columns = target type; 'ok' means the implicit conversion is accepted\n")
print(f"{'':<10}" + "".join(f"{t:<10}" for t in TYPES))
for s in TYPES:
    print(f"{s:<10}" + "".join(f"{results[(s,d)]:<10}" for d in TYPES))

accepted = [(s, d) for (s, d), v in results.items() if v == "ok" and s != d]
print(f"\nimplicit conversions accepted (excluding identity): {len(accepted)}")
for s, d in accepted:
    print(f"  {s} -> {d}")
bad = [(s, d, v) for (s, d), v in results.items() if v in ("ICE", "BADCPP", "TIMEOUT")]
if bad:
    print(f"\nhard failures: {len(bad)}")
    for s, d, v in bad:
        print(f"  {v} {s} -> {d}")
