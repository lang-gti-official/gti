#!/bin/bash
# Compile and run every coverage program; classify the outcome.
#
# Usage: tests/coverage/run_coverage.sh <path-to-gti> [output-dir]
#
# This corpus exists to exercise breadth of syntax and stdlib surface, not to
# demonstrate features. Programs are expected to be valid GTI; anything that
# does not compile and run is a compiler finding, recorded in FINDINGS.md.
set -u
GTI="${1:?usage: run_coverage.sh <path-to-gti> [output-dir]}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${2:-${TMPDIR:-/tmp}/gti-coverage-out}"
mkdir -p "$OUT"

pass=0; ice=0; rej=0; silent=0
: > "$OUT/pass.txt"; : > "$OUT/ice.txt"; : > "$OUT/rejected.txt"; : > "$OUT/silent.txt"

for src in "$DIR"/*.gti; do
  name="$(basename "$src" .gti)"
  profile=""
  grep -q '@coverage-profile: concurrent' "$src" && profile="--execution-profile concurrent"
  if "$GTI" $profile "$src" -o "$OUT/$name" > "$OUT/$name.err" 2>&1; then
    if "$OUT/$name" > "$OUT/$name.out" 2>&1; then
      pass=$((pass + 1)); echo "$name" >> "$OUT/pass.txt"
    else
      rej=$((rej + 1)); echo "$name (runtime failure)" >> "$OUT/rejected.txt"
    fi
  elif grep -q 'GTI-B0001' "$OUT/$name.err"; then
    ice=$((ice + 1))
    echo "$name :: $(grep -o 'GTI-B0001.*' "$OUT/$name.err" | head -1 | cut -c1-160)" >> "$OUT/ice.txt"
  elif [ ! -s "$OUT/$name.err" ]; then
    silent=$((silent + 1)); echo "$name :: no diagnostic emitted" >> "$OUT/silent.txt"
  else
    rej=$((rej + 1))
    echo "$name :: $(grep -oE 'error\[GTI-[A-Z0-9]+\]: .*' "$OUT/$name.err" | head -1 | cut -c1-140)" >> "$OUT/rejected.txt"
  fi
done

echo "pass=$pass ice=$ice silent=$silent rejected=$rej"
echo "results in $OUT"
[ "$ice" -eq 0 ] && [ "$silent" -eq 0 ] && [ "$rej" -eq 0 ]
