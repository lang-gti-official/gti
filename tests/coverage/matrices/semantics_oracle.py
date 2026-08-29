#!/usr/bin/env python3
"""Check that GTI computes the right answers, not just that it compiles.

The existing expression differential covers integer arithmetic against an
independent evaluator. This covers the surface it does not reach: container
and string operations, float formatting, integer conversions at boundaries,
and control-flow results — each with an expected value computed here.
"""
import pathlib, subprocess, sys

GTI = sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/gti-cov/build/gti"
OUT = pathlib.Path("/tmp/sem"); OUT.mkdir(exist_ok=True)

# name -> (body producing prints, expected stdout)
CASES = {
    "vector-basics": ("""
  mut std::vector<int32_t> v = std::vector<int32_t>();
  v.push_back(3); v.push_back(1); v.push_back(2);
  std::print(int32_t(v.size())); std::print(" ");
  std::print(v[std::size_t(0)]); std::print(v[std::size_t(2)]); std::print(" ");
  v.pop_back();
  std::print(int32_t(v.size())); std::print(" ");
  v.clear();
  std::println(v.empty() ? 1 : 0);
""", "3 32 2 1\n"),
    "vector-sort": ("""
  mut std::vector<int32_t> v = std::vector<int32_t>();
  v.push_back(5); v.push_back(1); v.push_back(4); v.push_back(2); v.push_back(3);
  std::sort(v);
  for (mut std::size_t i = std::size_t(0); i < v.size(); i++) { std::print(v[i]); }
  std::println("");
""", "12345\n"),
    "string-ops": ("""
  mut std::string s = std::string("ab");
  s.append("cd");
  s.push_back('e');
  std::print(s.view()); std::print(" ");
  std::print(int32_t(s.size())); std::print(" ");
  std::println(s.empty() ? 1 : 0);
""", "abcde 5 0\n"),
    "int-boundaries": ("""
  std::print(int32_t(int8_t(-128))); std::print(" ");
  std::print(int32_t(uint8_t(255))); std::print(" ");
  std::print(int32_t(int16_t(-32768))); std::print(" ");
  std::print(int32_t(uint16_t(65535))); std::print(" ");
  std::print(-2147483648); std::print(" ");
  std::println(2147483647);
""", "-128 255 -32768 65535 -2147483648 2147483647\n"),
    "wrapping": ("""
  std::print(int32_t(std::wrapping_add(uint8_t(250), uint8_t(10)))); std::print(" ");
  std::print(std::wrapping_add(2147483647, 1)); std::print(" ");
  std::println(std::wrapping_sub(-2147483648, 1));
""", "4 -2147483648 2147483647\n"),
    "integer-division": ("""
  std::print(7 / 2); std::print(" ");
  std::print(-7 / 2); std::print(" ");
  std::print(7 % 3); std::print(" ");
  std::println(-7 % 3);
""", "3 -3 1 -1\n"),
    "bit-ops": ("""
  uint32_t b = uint32_t(10);
  std::print(int32_t(b & uint32_t(6))); std::print(" ");
  std::print(int32_t(b | uint32_t(5))); std::print(" ");
  std::print(int32_t(b ^ uint32_t(15))); std::print(" ");
  std::print(int32_t(b << uint32_t(2))); std::print(" ");
  std::println(int32_t(b >> uint32_t(1)));
""", "2 15 5 40 5\n"),
    "float-text": ("""
  std::print(1.5); std::print(" ");
  std::print(-3.25); std::print(" ");
  std::println(0.0);
""", "1.500000 -3.250000 0.000000\n"),
    "control-flow": ("""
  mut int32_t fib_a = 0; mut int32_t fib_b = 1;
  for (mut int32_t i = 0; i < 10; i++) { mut int32_t t = fib_a + fib_b; fib_a = fib_b; fib_b = t; }
  std::print(fib_a); std::print(" ");
  mut int32_t hits = 0;
  for (mut int32_t r = 0; r < 5; r++) { if (r == 2) { continue; } if (r == 4) { break; } hits = hits + 1; }
  std::println(hits);
""", "55 3\n"),
    "span-sum": ("""
  mut std::vector<int32_t> v = std::vector<int32_t>();
  v.push_back(1); v.push_back(2); v.push_back(3); v.push_back(4);
  std::span<int32_t> all = v.as_span();
  std::span<int32_t> mid = all.subspan(std::size_t(1), std::size_t(2));
  mut int32_t t = 0;
  for (mut std::size_t i = std::size_t(0); i < mid.size(); i++) { t = t + mid[i]; }
  std::print(t); std::print(" ");
  std::println(int32_t(all.size()));
""", "5 4\n"),
    "algorithms": ("""
  mut std::vector<int32_t> v = std::vector<int32_t>();
  v.push_back(2); v.push_back(4); v.push_back(7);
  std::print(std::all_of(v.begin(), v.end(), [](int32_t x) -> bool { return x > 0; }) ? 1 : 0);
  std::print(std::any_of(v.begin(), v.end(), [](int32_t x) -> bool { return x % 2 == 1; }) ? 1 : 0);
  std::print(std::none_of(v.begin(), v.end(), [](int32_t x) -> bool { return x > 100; }) ? 1 : 0);
  std::print(" ");
  std::print(int32_t(std::count_if(v.begin(), v.end(), [](int32_t x) -> bool { return x >= 4; })));
  std::print(" ");
  std::print(std::min(3, 8)); std::print(std::max(3, 8)); std::println(std::clamp(11, 0, 9));
""", "111 2 389\n"),
    "expected-flow": ("""
  mut auto ok = divide(10, 2);
  mut auto bad = divide(1, 0);
  std::print(ok.has_value() ? ok.value() : -1); std::print(" ");
  std::println(bad.has_value() ? 0 : 1);
""", "5 1\n"),
}

HEAD = """#include <std/vector>
#include <std/string>
#include <std/algorithm>
#include <std/span>
#include <std/numeric>

enum class calc_errc { divide_by_zero };
expected<int32_t, calc_errc> divide(int32_t a, int32_t b) {
  if (b == 0) { return unexpected(calc_errc::divide_by_zero); }
  return a / b;
}
"""

fails = []
for name, (body, expected) in CASES.items():
    src = OUT / f"{name}.gti"
    src.write_text(HEAD + f"int main() {{{body}  return 0;\n}}\n")
    build = subprocess.run([GTI, str(src), "-o", str(src) + ".bin"],
                           capture_output=True, text=True, timeout=180)
    if build.returncode != 0:
        out = build.stdout + build.stderr
        code = [t for t in out.split() if t.startswith("error[GTI-")]
        verdict = code[0].strip("error[]:") if code else "build failed"
        print(f"  {name:<20} BUILD: {verdict}")
        fails.append((name, f"build: {verdict}"))
        continue
    run = subprocess.run([str(src) + ".bin"], capture_output=True, text=True, timeout=60)
    if run.stdout != expected:
        print(f"  {name:<20} WRONG OUTPUT")
        print(f"      expected {expected!r}")
        print(f"      got      {run.stdout!r}")
        fails.append((name, "wrong output"))
    else:
        print(f"  {name:<20} ok")

print(f"\nwrong or unbuildable: {len(fails)} / {len(CASES)}")
