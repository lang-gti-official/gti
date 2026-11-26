#!/usr/bin/env python3
"""Sweep threading constructs under --execution-profile concurrent.

Threads plus ownership is where a language's hardest invariants live, and this
profile is exercised by exactly one shipped example.
"""
import sys

import matrix_support

M = matrix_support.Matrix(__doc__.split("\n\n")[0], 'concurrency')
OUT = M.out

HEAD = """import std.thread;
import std.vector;
import std.string;

class Res { mut int32_t n = 0; public: Res(int32_t n) : n(n) {} int32_t get() { return this.n; } };
"""

CASES = {
    "spawn-join":        "mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); mut auto j = t.join(); total = j.has_value() ? 1 : 0;",
    "joinable-before":   "mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); total = t.joinable() ? 1 : 0; [[discard]] t.join();",
    "joinable-after":    "mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); [[discard]] t.join(); total = t.joinable() ? 0 : 1;",
    "capture-value":     "int32_t seed = 7; mut auto s = std::thread::start([seed]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); [[discard]] t.join(); total = seed;",
    "two-threads":       "mut auto a = std::thread::start([]() -> void {}); mut auto b = std::thread::start([]() -> void {}); if (!a) { return 1; } if (!b) { return 2; } mut std::thread ta = std::move(a).value(); mut std::thread tb = std::move(b).value(); [[discard]] ta.join(); [[discard]] tb.join(); total = 2;",
    "spawn-in-loop":     "for (mut int32_t i = 0; i < 3; i++) { mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); [[discard]] t.join(); total = total + 1; }",
    "no-join-drop":      "mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); total = 1;",
    "thread-in-scope":   "{ mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); [[discard]] t.join(); } total = 1;",
    "move-thread":       "mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); mut std::thread u = std::move(t); [[discard]] u.join(); total = 1;",
    "capture-owner":     "mut Res r = Res(4); mut auto s = std::thread::start([]() -> void {}); if (!s) { return 1; } mut std::thread t = std::move(s).value(); [[discard]] t.join(); total = r.get();",
}

print(f"{'case':<20}{'concurrent':<14}{'default profile':<18}")
print("-" * 52)
hard = []
for name, body in CASES.items():
    src = OUT / f"{name}.gti"
    src.write_text(HEAD + f"int main() {{\n  mut int32_t total = 0;\n  {body}\n  std::println(total);\n  return 0;\n}}\n")
    c = M.classify(src, key=f"{name}/concurrent", flags=("--execution-profile", "concurrent"))
    d = M.classify(src, key=f"{name}/default")
    print(f"{name:<20}{c:<14}{d:<18}")
    if matrix_support.hard(c):
        hard.append((name, c))

print(f"\nhard failures under concurrent: {len(hard)} / {len(CASES)}")
for n, v in hard:
    print(f"  {v:<10} {n}")
sys.exit(M.finish(hard))
