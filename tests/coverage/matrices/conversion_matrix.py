#!/usr/bin/env python3
"""Sweep implicit conversion source type x target type.

GTI permits bounded conversions in assignment contexts, while calls match
exact argument types. This records the accepted assignment relation; integer
to float/double may round (S12 is an audited contract, not an open defect).
"""
import sys

import matrix_support

M = matrix_support.Matrix(__doc__.split("\n\n")[0], 'conversion')
OUT = M.out

TYPES = ["int8_t", "int16_t", "int32_t", "int64_t",
         "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double"]

results = {}
for src_t in TYPES:
    for dst_t in TYPES:
        p = OUT / f"{src_t}__{dst_t}.gti"
        p.write_text(
            "class Anchor { public: int32_t id() { return 0; } };\n"
            f"int main() {{ mut {src_t} a = {src_t}(1); mut {dst_t} b = a; return 0; }}\n")
        results[(src_t, dst_t)] = M.classify(p, execute=False)

print("rows = source type, columns = target type; 'ok' means the implicit conversion is accepted\n")
print(f"{'':<10}" + "".join(f"{t:<10}" for t in TYPES))
for s in TYPES:
    print(f"{s:<10}" + "".join(f"{results[(s,d)]:<10}" for d in TYPES))

accepted = [(s, d) for (s, d), v in results.items() if v == "ok" and s != d]
print(f"\nimplicit conversions accepted (excluding identity): {len(accepted)}")
for s, d in accepted:
    print(f"  {s} -> {d}")
bad = [(s, d, v) for (s, d), v in results.items() if matrix_support.hard(v)]
if bad:
    print(f"\nhard failures: {len(bad)}")
    for s, d, v in bad:
        print(f"  {v} {s} -> {d}")
sys.exit(M.finish(bad))
