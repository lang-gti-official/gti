#!/usr/bin/env python3
"""Sweep threading constructs under --execution-profile concurrent.

Threads plus ownership is where a language's hardest invariants live, and this
profile is exercised by exactly one shipped example.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/concmx"); OUT.mkdir(exist_ok=True)

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

def classify(path, concurrent):
    cmd = [GTI]
    if concurrent:
        cmd += ["--execution-profile", "concurrent"]
    cmd += [str(path), "-o", str(path) + ".bin"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        run = subprocess.run([str(path) + ".bin"], capture_output=True, text=True, timeout=60)
        return "ok" if run.returncode == 0 else f"RUN{run.returncode}"
    if "GTI-B0001" in out: return "ICE"
    if "Undefined symbols" in out: return "LINK"
    if "error:" in out and "error[GTI-" not in out: return "BADCPP"
    if not out.strip(): return "SILENT"
    code = [t for t in out.split() if t.startswith("error[GTI-")]
    return code[0].strip("error[]:").replace("GTI-", "") if code else f"exit{r.returncode}"

print(f"{'case':<20}{'concurrent':<14}{'default profile':<18}")
print("-" * 52)
hard = []
for name, body in CASES.items():
    src = OUT / f"{name}.gti"
    src.write_text(HEAD + f"int main() {{\n  mut int32_t total = 0;\n  {body}\n  std::println(total);\n  return 0;\n}}\n")
    try:
        c = classify(src, True)
    except subprocess.TimeoutExpired:
        c = "TIMEOUT"
    try:
        d = classify(src, False)
    except subprocess.TimeoutExpired:
        d = "TIMEOUT"
    print(f"{name:<20}{c:<14}{d:<18}")
    if c in ("ICE", "BADCPP", "SILENT", "LINK", "TIMEOUT") or c.startswith("RUN"):
        hard.append((name, c))

print(f"\nhard failures under concurrent: {len(hard)} / {len(CASES)}")
for n, v in hard:
    print(f"  {v:<10} {n}")
