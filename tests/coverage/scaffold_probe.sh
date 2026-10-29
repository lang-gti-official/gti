#!/bin/bash
# Probe representative stdlib operations the surface manifest marks "scaffold".
# Each should report a GTI-level diagnostic; record what it actually reports.
mkdir -p "${TMPDIR:-/tmp}/gti-scaffold-probe"
G="${1:?usage: scaffold_probe.sh <path-to-gti>}"
probe() {
  name="$1"; body="$2"
  printf '%s\n' "$body" > ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.gti
  if $G ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.gti -o ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.bin >${TMPDIR:-/tmp}/gti-scaffold-probe/$name.err 2>&1; then
    printf "%-24s %s\n" "$name" "BUILDS"
  elif grep -q "GTI-B0001" ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.err; then
    printf "%-24s %s\n" "$name" "ICE (GTI-B0001)"
  elif grep -q "Undefined symbols" ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.err; then
    printf "%-24s %s\n" "$name" "LINK ERROR (C++ mangled symbol)"
  elif grep -q "error\[GTI-" ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.err; then
    printf "%-24s %s\n" "$name" "gti diagnostic: $(grep -oE 'error\[GTI-[A-Z0-9]+\]' ${TMPDIR:-/tmp}/gti-scaffold-probe/$name.err | head -1)"
  else
    printf "%-24s %s\n" "$name" "SILENT (no diagnostic)"
  fi
}
probe cmath_sqrt 'import std.cmath;
class A { public: int32_t i() { return 0; } };
int main() { float v = 2.0; std::println(int32_t(std::sqrt(v))); return 0; }'
probe cmath_floor 'import std.cmath;
class A { public: int32_t i() { return 0; } };
int main() { float v = 2.7; std::println(int32_t(std::floor(v))); return 0; }'
probe algorithm_transform 'import std.vector;
import std.algorithm;
class A { public: int32_t i() { return 0; } };
int main() {
  mut std::vector<int32_t> a = std::vector<int32_t>();
  mut std::vector<int32_t> b = std::vector<int32_t>();
  a.push_back(1);
  std::transform(a, b, [](int32_t x) -> int32_t { return x + 1; });
  std::println(int32_t(b.size()));
  return 0; }'
probe string_swap 'import std.string;
int main() {
  mut std::string a = std::string("x");
  mut std::string b = std::string("y");
  a.swap(b);
  std::println(a.view()); return 0; }'
probe utility_swap 'import std.utility;
class A { public: int32_t i() { return 0; } };
int main() { mut int32_t a = 1; mut int32_t b = 2; std::swap(a, b); std::println(a); return 0; }'
probe utility_exchange 'import std.utility;
class A { public: int32_t i() { return 0; } };
int main() { mut int32_t a = 1; int32_t old = std::exchange(a, 5); std::println(old + a); return 0; }'
probe vector_swap 'import std.vector;
class A { public: int32_t i() { return 0; } };
int main() {
  mut std::vector<int32_t> a = std::vector<int32_t>();
  mut std::vector<int32_t> b = std::vector<int32_t>();
  a.swap(b);
  std::println(int32_t(a.size())); return 0; }'
probe memory_shared_ptr 'import std.memory;
class A { public: int32_t i() { return 0; } };
int main() { mut auto p = std::make_shared<int32_t>(4); std::println(*p); return 0; }'
probe optional_use 'import std.optional;
class A { public: int32_t i() { return 0; } };
int main() { mut std::optional<int32_t> o = std::optional<int32_t>(1); std::println(o.has_value() ? 1 : 0); return 0; }'
probe list_use 'import std.list;
class A { public: int32_t i() { return 0; } };
int main() { mut std::list<int32_t> l = std::list<int32_t>(); std::println(l.empty() ? 1 : 0); return 0; }'
probe hash_builtin 'import std.functional;
class A { public: int32_t i() { return 0; } };
int main() { std::hash<int32_t> h = std::hash<int32_t>(); mut int32_t v = 1; std::println(int32_t(h(v))); return 0; }'
