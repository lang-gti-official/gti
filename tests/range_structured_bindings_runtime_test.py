#!/usr/bin/env python3
"""Execute owning and borrowed structured range bindings on LLVM."""
import pathlib
import subprocess
import sys
import tempfile
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("range_structured_bindings", "range_structured_cleanup"),
         cases={"range_structured_cleanup": [((), (0, "1\n2\n3\n", ""))]})

    prefix = """import std.string;
import std.vector;
import std.utility;
struct Entry {
  mut std::string key;
  mut int value;
  Entry(std::string key, int value) : key(std::move(key)), value(value) {}
};
int main() {
  mut std::vector<Entry> entries{};
  entries.emplace_back(std::string("key"), 1);
"""
    invalid = {
        "implicit-copy": "for (auto [key, value] : entries) {}",
        "partial-move": "for (auto& [key, value] : entries) { auto moved = std::move(key); }",
        "owner-invalidation": "for (auto& [key, value] : entries) { entries.clear(); int size = int(key.size()); }",
        "immutable-field": "struct Frozen { int a; int b; Frozen():a(1),b(2){} }; mut Frozen frozen[1] = {Frozen()}; for (mut auto& [a, b] : frozen) {}",
        "mutable-invalidation": "for (mut auto& [key, value] : entries) { entries.clear(); value += 1; }",
        "mutable-alias": "for (mut auto& [key, value] : entries) { auto& held = value; value += 1; int copy = held; }",
        "immutable-owner": "auto& readonly = entries; for (mut auto& [key, value] : readonly) { value += 1; }",
        "wrong-arity": "for (auto& [key] : entries) {}",
    }
    with tempfile.TemporaryDirectory(prefix="gti-range-pattern-negative-") as directory:
        scratch = pathlib.Path(directory)
        debug_binary = scratch / "debug-range"
        fixture = pathlib.Path(__file__).parent / "fixtures/range_structured_bindings.gti"
        result = subprocess.run([sys.argv[1], str(fixture), "-O0", "-g", "--backend", "llvm",
                                 "-o", str(debug_binary)], capture_output=True, text=True, timeout=90)
        assert result.returncode == 0, result.stdout + result.stderr
        # The declaration test checks alias debug facts; execute the native
        # debug build here to cover their LLVM lowering and emitted artifact.
        result = subprocess.run([str(debug_binary)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        for name, body in invalid.items():
            source = scratch / (name + ".gti")
            source.write_text(prefix + body + " return 0; }")
            result = subprocess.run([sys.argv[1], str(source), "--emit-mir", "-o",
                                     str(scratch / (name + ".mir"))],
                                    capture_output=True, text=True, timeout=90)
            diagnostics = result.stdout + result.stderr
            assert result.returncode != 0 and "GTI-S" in diagnostics, (name, diagnostics)
            assert "GTI-B0001" not in diagnostics, (name, diagnostics)
