#!/usr/bin/env python3
"""Native places/loans/array/text execution; C++ is a differential endpoint."""

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


ARRAYS = r'''
int snapshot(mut int data[3]) { data[0] = 99; return data[1]; }
void set(mut int& value, int next) { value = next; }
mut int& relay(mut int& value) { return value; }
int& echo(int& value) { return value; }
int selected_index(bool last) { return last ? 2 : 0; }
int main() {
  mut int data[3] = {1, 2, 3};
  set(data[1], 7);
  relay(data[0]) = 4;
  relay(data[selected_index(false)]) = 4;
  relay(data[selected_index(true)]) = 3;
  { mut int& element = data[2]; element += 5; }
  if (echo(data[2]) != 8 || snapshot(data) != 7 || data[0] != 4) { return 1; }
  mut int index = 0;
  data[index++] += 3;
  if (index != 1 || data[0] != 7) { return 2; }
  mut int grid[2][2] = {{1, 2}, {3, 4}};
  grid[1][0] = data[0];
  if (grid[1][0] != 7 || grid[0][1] != 2) { return 3; }
  mut bool flags[3] = {true, false, false};
  flags[1] = flags[0];
  if (!flags[1] || flags[2]) { return 4; }
  int copy[2][2] = grid;
  grid[1][0] = 9;
  if (copy[1][0] != 7 || grid.size() != uint64_t(2)) { return 5; }
  return 0;
}
'''

PASSIVE_ARRAYS = r'''
int snapshot(mut int data[3]) { data[0] = 99; return data[1]; }
int main() {
  mut int data[3] = {1, 2, 3};
  if (snapshot(data) != 2 || data[0] != 1) { return 1; }
  mut int index = 0;
  data[index++] += 3;
  if (index != 1 || data[0] != 4) { return 2; }
  mut int grid[2][2] = {{1, 2}, {3, 4}};
  grid[1][0] = data[0];
  if (grid[1][0] != 4 || grid[0][1] != 2) { return 3; }
  mut bool flags[3] = {true, false, false};
  flags[1] = flags[0];
  if (!flags[1] || flags[2]) { return 4; }
  int copy[2][2] = grid;
  grid[1][0] = 9;
  if (copy[1][0] != 4) { return 5; }
  return 0;
}
'''

REFERENCES = r'''
mut int global = 9;
mut int& root() { return global; }
mut int& relay(mut int& value) { return value; }
mut int& checked(mut int& value, int divisor) {
  value = value / divisor;
  return value;
}
int main() {
  root() += 1;
  mut int local = 3;
  { mut int& alias = checked(local, 1); relay(alias) += 4; }
  if (local != 7 || global != 10) { return 1; }
  mut int* pointer = &local;
  mut int other = 11;
  { mut int*& slot = pointer; slot = &other; }
  unsafe { *pointer = 12; }
  if (local != 7 || other != 12) { return 2; }
  return 0;
}
'''

BASIC_REFERENCES = r'''
mut int global = 9;
mut int& root() { return global; }
mut int& relay(mut int& value) { return value; }
mut int& checked(mut int& value, int divisor) { value = value / divisor; return value; }
int main() {
  root() += 1;
  mut int local = 3;
  { mut int& alias = checked(local, 1); relay(alias) += 4; }
  if (local != 7 || global != 10) { return 1; }
  return 0;
}
'''

GLOBAL_ARRAYS = r'''
#include <std/string_view>
mut int grid[2][2] = {{1, 2}, {3, 4}};
bool flags[2] = {true, false};
std::string_view label = "a\0b";
mut int* pointer = nullptr;
void update() { grid[1][0] = 7; }
int main() {
  update();
  if (grid[1][0] != 7 || grid[0][1] != 2 || !flags[0] || flags[1]) { return 1; }
  if (label.size() != uint64_t(3) || label[1] != '\0' || pointer != nullptr) { return 2; }
  pointer = &grid[1][0];
  unsafe { if (*pointer != 7) { return 3; } }
  return 0;
}
'''

POINTERS = r'''
using Erased = void*;
using Address = int*;
int main() {
  mut int data[4] = {10, 20, 30, 40};
  mut int* cursor = &data[0];
  int* absent = nullptr;
  if (absent != nullptr || cursor == nullptr) { return 1; }
  int* read = cursor;
  unsafe {
    cursor += 2;
    if (*cursor != 30 || cursor - read != 2) { return 2; }
    cursor--;
    cursor[1] = 31;
    *cursor = 21;
    int* end = cursor + uint64_t(3);
    if (end - cursor != 3 || read - end != -4) { return 3; }
    cursor -= 1;
    if (*cursor != 10) { return 4; }
  }
  void* erased = Erased(cursor);
  int* restored = Address(erased);
  if (restored != cursor || data[1] != 21 || data[2] != 31) { return 5; }
  int** outer = &cursor;
  unsafe { *outer = &data[3]; if (**outer != 40) { return 6; } }
  return 0;
}
'''

# A sealed integral enum is stored as its underlying integer: a vector of one,
# raw element reads/writes, pointer arithmetic and local array indexing.
INTEGRAL_ENUM_POINTERS = r'''
#include <std/vector>
enum class Event { dial, ack, data, hangup, reset };
enum class Phase : uint8_t { idle = 1, open = 3, closed = 9 };
int main() {
  mut std::vector<Event> script{};
  script.push_back(Event::dial);
  script.push_back(Event::data);
  script.push_back(Event::hangup);
  if (script.size() != uint64_t(3) || script[uint64_t(1)] != Event::data) { return 1; }
  mut Phase phases[3] = {Phase::idle, Phase::open, Phase::closed};
  mut Phase* first = &phases[0];
  Phase* last = &phases[2];
  unsafe {
    first[1] = Phase::closed;
    if (*last != Phase::closed || last - first != 2) { return 2; }
    first += 1;
    if (*first != Phase::closed) { return 3; }
  }
  if (phases[1] != Phase::closed) { return 4; }
  return 0;
}
'''

PRIMITIVE_POINTERS = r'''
int main() {
  mut int values[3] = {10, 20, 30};
  mut int* first = &values[0];
  int* middle = &values[1];
  unsafe {
    first[1] = 21;
    *middle = *middle + 1;
    if (middle[-1] != 10 || first[2] != 30) { return 1; }
    first += 2;
    if (first - middle != 1 || *(first - 1) != 22) { return 3; }
    first--;
    ++first;
    first -= 2;
    if (first + uint64_t(1) != middle || uint64_t(1) + first != middle || first - middle != -1) { return 4; }
  }
  if (values[1] != 22) { return 2; }
  return 0;
}
'''

TEXT = r'''
#include <std/string_view>
std::string_view relay(std::string_view value) { return value; }
std::string_view choose(bool condition) { return condition ? "a\0bc" : "xy"; }
int inspect(std::string_view value, uint64_t index) {
  if (value.empty()) { return 0; }
  if (value.size() == uint64_t(4) && value[index] == 'b') { return 1; }
  return 2;
}
int main() {
  std::string_view text = "a\0bc";
  std::string_view selected = choose(true);
  std::string_view other = choose(false);
  std::string_view empty = "";
  if (inspect(text, uint64_t(2)) != 1 || !empty.empty()) { return 1; }
  if (text[1] != '\0' || text[3] != 'c') { return 2; }
  if (text != "a\0bc" || text == "a\0bx" || text == "a\0b" || empty != "") { return 4; }
  if (selected != text || other.size() != uint64_t(2)) { return 5; }
  auto utf8 = "é";
  if (utf8.size() != uint64_t(2) || uint8_t(utf8[0]) != uint8_t(195)
      || uint8_t(utf8[1]) != uint8_t(169)) { return 3; }
  return 0;
}
'''

NESTED_TEXT_RETURN = r'''
#include <std/string_view>
std::string_view relay(std::string_view value) { return value; }
int inspect(std::string_view value) {
  if (value.size() != uint64_t(4) || value[1] != '\0' || value[3] != 'c') { return 1; }
  return 0;
}
int main() { std::string_view text = "a\0bc"; return inspect(relay(text)); }
'''

OWNER_ADDRESSES = r'''
#include <std/memory>
[[borrowed_from(owner)]] unsafe mut int& claim(int* address, mut int& owner) {
  unsafe { return *address; }
}
bool present(c_string text) { return text != nullptr; }
int inspect_owner_view(std::string_view text) {
  if (text.size() != uint64_t(3) || text[2] != 'b') { return 1; }
  return 0;
}
int main() {
  mut int value = 7;
  unsafe { claim(&value, value) += 2; }
  if (value != 9) { return 1; }
  {
    mut std::erased_object& erased = std::erase_mut(value);
    unsafe { std::restore_mut<int>(erased) += 3; }
  }
  if (value != 12) { return 2; }
  char bytes[4] = {'a', '\0', 'b', '\0'};
  unsafe {
    auto text = std::assume_string_view(&bytes[0], uint64_t(3), bytes);
    if (inspect_owner_view(text) != 0) { return 6; }
    if (text.size() != uint64_t(3) || text[2] != 'b' || text != "a\0b") { return 3; }
    c_string ctext = std::assume_c_string(&bytes[0], bytes);
    if (!present(ctext)) { return 4; }
  }
  auto literal_alias = "hello";
  if (!present("hello") || !present(literal_alias) || present(nullptr)) { return 5; }
  return 0;
}
'''

ERASED_REFERENCES = r'''
#include <std/memory>
struct Record { mut int value = 7; };
int main() {
  mut Record object{};
  {
    std::erased_object& erased = std::erase_ref(object);
    unsafe { if (std::restore_ref<Record>(erased).value != 7) { return 1; } }
  }
  {
    mut std::erased_object& erased = std::erase_mut(object);
    unsafe { std::restore_mut<Record>(erased).value += 5; }
  }
  return object.value - 12;
}
'''

ERASED_REFERENCE_CLEANUP = r'''
#include <std/memory>
mut int destroyed = 0;
struct Record {
  mut int value = 7;
  ~Record() { destroyed += this.value; }
};
int main() {
  {
    mut Record object{};
    {
      std::erased_object& erased = std::erase_ref(object);
      unsafe { if (std::restore_ref<Record>(erased).value != 7) { return 1; } }
    }
    {
      mut std::erased_object& erased = std::erase_mut(object);
      unsafe { std::restore_mut<Record>(erased).value += 5; }
    }
    if (destroyed != 0) { return 2; }
  }
  return destroyed == 12 ? 0 : 3;
}
'''

RAW_CLAIMS = r'''
[[borrowed_from(owner)]] unsafe mut int& claim(int* address, mut int& owner) {
  unsafe { return *address; }
}
int main() {
  mut int value = 7;
  unsafe { claim(&value, value) += 2; }
  return value - 9;
}
'''

UNSAFE_WRAPPER = r'''
#include <std/string_view>
unsafe std::string_view different(const char* address, uint64_t count, int& owner) {
  return "not the address";
}
unsafe int identity(int value) { return value; }
int main() {
  char bytes[3] = {'a', 'b', 'c'};
  int owner = 0;
  unsafe {
    auto text = different(&bytes[0], uint64_t(3), owner);
    if (text != "not the address") { return 1; }
    return identity(0);
  }
}
'''

CONDITIONAL_REFERENCES = r'''
[[borrowed_from(owner)]] unsafe int& choose<Owner>(
    const int* left, const int* right, bool last, Owner& owner) {
  unsafe {
    if (last) { return *right; }
    return *left;
  }
}
int main() {
  int values[3] = {3, 7, 11};
  unsafe {
    int& first = choose(&values[0], &values[2], false, values);
    int& last = choose(&values[0], &values[2], true, values);
    if (first != 3 || last != 11) { return 1; }
  }
  return 0;
}
'''


# A fixed-array extent query is a sealed constant: range-for and size() over
# class, owning-class and payload elements never read an element to count.
ARRAY_EXTENT_QUERIES = r'''
#include <std/string>
struct Counter {
  int count;
  Counter(int number) : count(number) {}
};
struct Slot {
  Counter value;
  Slot(int number) : value(Counter(number)) {}
};
struct Named {
  std::string text;
  Named(std::string_view spelling) : text(std::string(spelling)) {}
};
enum Plain { none, pair(int first, int second) };
int sum(Plain& value) {
  switch (value) {
  case Plain::none: return 0;
  case Plain::pair(first, second): return first + second;
  }
}
int main() {
  Slot slots[3] = {Slot(4), Slot(5), Slot(6)};
  mut int total = 0;
  for (auto& slot : slots) { total = total * 10 + slot.value.count; }
  if (total != 456 || slots.size() != uint64_t(3)) { return 1; }
  Slot grid[2][2] = {{Slot(1), Slot(2)}, {Slot(3), Slot(4)}};
  mut int weighted = 0;
  for (auto& row : grid) {
    for (auto& cell : row) { weighted = weighted * 10 + cell.value.count; }
  }
  if (weighted != 1234 || grid.size() != uint64_t(2) || grid[1].size() != uint64_t(2)) {
    return 2;
  }
  Named names[2] = {Named("ab"), Named("cde")};
  mut uint64_t letters = 0;
  for (auto& name : names) { letters = letters + name.text.view().size(); }
  if (letters != uint64_t(5) || names.size() != uint64_t(2)) { return 3; }
  Plain plains[3] = {Plain::pair(1, 2), Plain::none, Plain::pair(3, 4)};
  mut int sums = 0;
  for (auto& plain : plains) { sums = sums * 10 + sum(plain); }
  return sums == 307 && plains.size() == uint64_t(3) ? 0 : 4;
}
'''

# One-dimensional arrays of sealed integral enums use their fixed-width
# underlying integer storage in a user-defined owner and in std::array.
INTEGRAL_ENUM_ARRAYS = r'''
#include <std/array>
enum class phase : uint8_t { idle, loading, running };
enum class reach : int64_t { near = 7, far = 6000000000 };
phase next(phase current) {
  switch (current) {
  case phase::idle: return phase::loading;
  case phase::loading: return phase::running;
  case phase::running: return phase::idle;
  default: return phase::idle;
  }
}
// A user-defined owner: aggregate formation, the by-value array parameter
// and the field publication are the same rule std::array relies on.
struct Track {
  phase stops[3];
  Track(phase initial[3]) : stops(initial) {}
  phase at(uint64_t index) { return this.stops[index]; }
};
Track route(phase first) {
  phase second = next(first);
  return Track({first, second, next(second)});
}
std::array<phase, 3> cycle(phase first) {
  phase second = next(first);
  return std::array<phase, 3>({first, second, next(second)});
}
int main() {
  Track track = route(phase::running);
  if (track.at(uint64_t(0)) != phase::running || track.at(uint64_t(1)) != phase::idle ||
      track.at(uint64_t(2)) != phase::loading) {
    return 1;
  }
  std::array<phase, 3> states = cycle(phase::loading);
  if (states[uint64_t(0)] != phase::loading || states[uint64_t(1)] != phase::running ||
      states[uint64_t(2)] != phase::idle) {
    return 2;
  }
  mut std::array<phase, 3> copy = states;
  phase wrapped = next(copy.at(uint64_t(1)));
  copy.at(uint64_t(0)) = wrapped;
  if (states.at(uint64_t(0)) != phase::loading || copy.at(uint64_t(0)) != phase::idle ||
      copy.at(uint64_t(2)) != phase::idle) {
    return 3;
  }
  reach far = reach::far;
  std::array<reach, 2> reaches = std::array<reach, 2>({far, reach::near});
  if (reaches[uint64_t(0)] != reach::far || reaches[uint64_t(1)] != reach::near) { return 4; }
  return 0;
}
'''


def successes(root: Path) -> dict[str, str]:
    cases = {'passive-arrays': PASSIVE_ARRAYS, 'global-arrays': GLOBAL_ARRAYS,
             'arrays': ARRAYS, 'basic-references': BASIC_REFERENCES,
             'references': REFERENCES, 'pointers': POINTERS,
             'primitive-pointers': PRIMITIVE_POINTERS, 'text': TEXT,
             'owner-addresses': OWNER_ADDRESSES, 'raw-claims': RAW_CLAIMS,
             'erased-references': ERASED_REFERENCES,
             'erased-reference-cleanup': ERASED_REFERENCE_CLEANUP,
             'unsafe-wrapper': UNSAFE_WRAPPER,
             'conditional-references': CONDITIONAL_REFERENCES,
             'array-extent-queries': ARRAY_EXTENT_QUERIES,
             'integral-enum-arrays': INTEGRAL_ENUM_ARRAYS,
             'integral-enum-pointers': INTEGRAL_ENUM_POINTERS}
    cases['shipped-example'] = (
        root / 'examples/llvm/places-and-text.gti').read_text()
    cases['enum-references'] = (
        root / 'tests/fixtures/llvm_enum_references.gti').read_text()
    cases['enum-payload-borrow'] = (
        root / 'tests/fixtures/llvm_enum_payload_borrow.gti').read_text()
    cases['enum-raw-borrow'] = (
        root / 'tests/fixtures/llvm_enum_raw_borrow.gti').read_text()
    return cases


def native_only_cases() -> dict[str, str]:
    # The C++ representation vocabulary cannot currently transport nested text.
    # Native expectations come from GTI's byte/count contract.
    return {'nested-text-return': NESTED_TEXT_RETURN}


def failure_cases() -> dict[str, str]:
    return {
        'array-high': 'int get(int data[2], uint64_t i) { return data[i]; } int main() { return get({1, 2}, uint64_t(2)); }',
        'array-negative': 'int get(int data[2], int i) { return data[i]; } int main() { return get({1, 2}, -1); }',
        'array-borrow': 'int main() { mut int data[2] = {1, 2}; mut int i = 2; mut int& value = data[i]; return value; }',
        'nested-array': 'int get(int data[2][2], int i) { return data[1][i]; } int main() { return get({{1, 2}, {3, 4}}, 2); }',
        'text-high': '#include <std/string_view>\nchar get(std::string_view s, uint64_t i) { return s[i]; } int main() { [[discard]] get("ab", uint64_t(2)); return 0; }',
        'text-empty': '#include <std/string_view>\nchar get(std::string_view s, int i) { return s[i]; } int main() { [[discard]] get("", 0); return 0; }',
        'text-negative': '#include <std/string_view>\nchar get(std::string_view s, int i) { return s[i]; } int main() { [[discard]] get("ab", -1); return 0; }',
        'reference-failure': 'mut int& get(mut int& v, int n) { v = v / n; return v; } int main() { mut int x = 2; get(x, 0) = 4; return 0; }',
    }


def rejection_cases() -> dict[str, str]:
    return {
        'unsafe-dereference': 'int main() { int value = 1; int* pointer = &value; return *pointer; }',
        'unsafe-index': 'int main() { int values[2] = {1, 2}; int* pointer = &values[0]; return pointer[1]; }',
        'expired-reference': 'int& expired() { int local = 1; return local; } int main() { return expired(); }',
        'overlapping-mutable': 'int main() { mut int value = 1; mut int& first = value; mut int& second = value; return first + second; }',
        'read-only-reference': 'void change(int& value) { value = 2; } int main() { return 0; }',
        'erased-restore-unsafe': '#include <std/memory>\nint main() { int value = 1; std::erased_object& erased = std::erase_ref(value); return std::restore_ref<int>(erased); }',
        'erased-overlapping-mutable': '#include <std/memory>\nint main() { mut int value = 1; mut std::erased_object& first = std::erase_mut(value); mut std::erased_object& second = std::erase_mut(value); unsafe { return std::restore_mut<int>(first) + std::restore_mut<int>(second); } }',
        'erased-escape': '#include <std/memory>\nstd::erased_object& escape() { int value = 1; return std::erase_ref(value); } int main() { std::erased_object& erased = escape(); return 0; }',
        'enum-reference-escape': 'enum class E { first }; E& expired() { E value = E::first; return value; } int main() { return 0; }',
        'enum-reference-mutation': 'enum class E { first, second }; void change(E& value) { value = E::second; } int main() { return 0; }',
    }


def native_rejection_cases(root: Path) -> dict[str, str]:
    return {
        'array-comparison': 'int main() { int left[2] = {1, 2}; int right[2] = {1, 2}; return left == right ? 0 : 1; }',
        'dynamic-c-string': '#include <std/string_view>\nvoid consume(c_string text) {} void relay(std::string_view text) { consume(text); } int main() { relay("hello"); return 0; }',
        'nul-c-string': 'void consume(c_string text) {} int main() { consume("a\\0b"); return 0; }',
    }


def evidence_cases(root: Path) -> dict[str, str]:
    return {**successes(root), **native_only_cases(), **failure_cases()}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    cases = evidence_cases(root)
    profiles = {name: (name,) for name in cases}
    profiles['primitive-pointers'] += ('integral-enum-pointers',)
    profiles['array-high'] += ('integral-enum-pointers',)
    assert all(case in cases for profile in profiles.values() for case in profile)
    return profiles


def evidence_manifest(root: Path) -> dict:
    return build_manifest(
        'llvm_place_execution', evidence_cases(root), evidence_profiles(root),
        {'shipped-example': 'examples/llvm/places-and-text.gti',
         'enum-references': 'tests/fixtures/llvm_enum_references.gti',
         'enum-payload-borrow': 'tests/fixtures/llvm_enum_payload_borrow.gti',
         'enum-raw-borrow': 'tests/fixtures/llvm_enum_raw_borrow.gti'})


def list_evidence(root: Path) -> None:
    print(json.dumps(evidence_manifest(root), sort_keys=True))


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == '--list-evidence':
        list_evidence(Path(sys.argv[2]).resolve())
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else None
    env = dict(os.environ, GTI_STDLIB_DIR=str(root / 'stdlib'))
    success_cases = successes(root)
    # The production C++ backend currently rejects this valid nested text
    # transport in its representation vocabulary. The native expected result
    # comes from the source byte/count contract, not that backend limitation.
    native_only = native_only_cases()
    failures = failure_cases()
    rejections = rejection_cases()
    # Current semantics admits these despite the documented literal-only
    # contract. Do not reproduce the production backend's unchecked data()
    # conversion: native admission requires an exact NUL-free literal proof.
    native_rejections = native_rejection_cases(root)
    endpoints = (Endpoint('cpp', 'O3', 'c++20'), Endpoint('cpp', 'O3', 'c++23'),
                 *(Endpoint('llvm', mode) for mode in ('O0', 'O1', 'O2', 'O3')))
    if selection and selection not in success_cases and selection not in failures and selection not in native_only and selection not in rejections and selection not in native_rejections:
        raise ValueError(f'unknown execution case: {selection}')
    with tempfile.TemporaryDirectory(prefix='gti-llvm-places-') as directory:
        work = Path(directory)
        for name, source_text in {**success_cases, **failures}.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(source_text, encoding='utf-8')
            results = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f'{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}', source, endpoint, env)
                expected = 70 if name in failures else 0
                code = 'GTI-R0002' if name == 'reference-failure' else 'GTI-R0007'
                if result.error or result.returncode != expected or (expected == 70 and code not in result.stderr):
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            if any(result != results[0] for result in results):
                raise AssertionError(f'{name}: observable drift: {results}')
            print(f'{name}: C++20/23 O3 / LLVM O0-O3 passed', flush=True)
        for name, text in native_only.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            for endpoint in (Endpoint('llvm', mode) for mode in ('O0', 'O1', 'O2', 'O3')):
                result = execute(gti, root, work / f'{name}-{endpoint.optimization}', source, endpoint, env)
                if result.error or result.observable != (0, '', ''):
                    raise AssertionError(f'{name} {endpoint}: {result}')
            print(f'{name}: LLVM O0-O3 source-contract oracle passed (known C++ representation limitation)', flush=True)
        for name, text in rejections.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            for backend in ('cpp', 'llvm'):
                output = work / f'{name}-{backend}'
                result = subprocess.run([str(gti), str(source), '--backend', backend, '-o', str(output)],
                                        env=env, capture_output=True, text=True, timeout=30)
                if result.returncode != 65 or 'error[GTI-S' not in result.stderr or output.exists():
                    raise AssertionError(f'{name} {backend}: expected source safety rejection: {result.stderr}')
            print(f'{name}: both backends preserve source safety rejection', flush=True)
        for name, text in native_rejections.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            output = work / f'{name}-llvm'
            output.write_bytes(b'preserved output')
            for mode in ('-O0', '-O3'):
                result = subprocess.run([str(gti), str(source), '--backend', 'llvm', mode, '-o', str(output)],
                                        env=env, capture_output=True, text=True, timeout=30)
                if result.returncode != 65 or 'error[GTI-B0003]' not in result.stderr or output.read_bytes() != b'preserved output':
                    raise AssertionError(f'{name}: native proof gate did not fail closed: {result.stderr}')
                if name == 'array-comparison' and 'Shape.ArrayValueComparison' not in result.stderr:
                    raise AssertionError(f'{name}: scalar equality escaped preflight: {result.stderr}')
            print(f'{name}: native proof rejection preserves existing output', flush=True)
    print('LLVM places, selected references, passive arrays and counted text passed')


if __name__ == '__main__':
    main()
