#!/usr/bin/env python3
"""Native places/loans/array/text execution; C++ is a differential endpoint."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


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
mut int grid[2][2] = {{1, 2}, {3, 4}};
bool flags[2] = {true, false};
void update() { grid[1][0] = 7; }
int main() {
  update();
  if (grid[1][0] != 7 || grid[0][1] != 2 || !flags[0] || flags[1]) { return 1; }
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
    if (first + uint64_t(1) != middle || first - middle != -1) { return 4; }
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
    if (text.size() != uint64_t(3) || text[2] != 'b' || text != "a\0b") { return 3; }
    c_string ctext = std::assume_c_string(&bytes[0], bytes);
    if (!present(ctext)) { return 4; }
  }
  if (!present("hello") || present(nullptr)) { return 5; }
  return 0;
}
'''


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else None
    env = dict(os.environ, GTI_STDLIB_DIR=str(root / 'stdlib'))
    successes = {'passive-arrays': PASSIVE_ARRAYS, 'global-arrays': GLOBAL_ARRAYS, 'arrays': ARRAYS, 'basic-references': BASIC_REFERENCES, 'references': REFERENCES,
                 'pointers': POINTERS, 'primitive-pointers': PRIMITIVE_POINTERS, 'text': TEXT,
                 'owner-addresses': OWNER_ADDRESSES}
    successes['shipped-example'] = (root / 'examples/llvm/places-and-text.gti').read_text()
    # The production C++ backend currently rejects this valid nested text
    # transport in its representation vocabulary. The native expected result
    # comes from the source byte/count contract, not that backend limitation.
    native_only = {'nested-text-return': NESTED_TEXT_RETURN}
    failures = {
        'array-high': 'int get(int data[2], uint64_t i) { return data[i]; } int main() { return get({1, 2}, uint64_t(2)); }',
        'array-negative': 'int get(int data[2], int i) { return data[i]; } int main() { return get({1, 2}, -1); }',
        'array-borrow': 'int main() { mut int data[2] = {1, 2}; mut int i = 2; mut int& value = data[i]; return value; }',
        'nested-array': 'int get(int data[2][2], int i) { return data[1][i]; } int main() { return get({{1, 2}, {3, 4}}, 2); }',
        'text-high': '#include <std/string_view>\nchar get(std::string_view s, uint64_t i) { return s[i]; } int main() { [[discard]] get("ab", uint64_t(2)); return 0; }',
        'text-empty': '#include <std/string_view>\nchar get(std::string_view s, int i) { return s[i]; } int main() { [[discard]] get("", 0); return 0; }',
        'text-negative': '#include <std/string_view>\nchar get(std::string_view s, int i) { return s[i]; } int main() { [[discard]] get("ab", -1); return 0; }',
        'reference-failure': 'mut int& get(mut int& v, int n) { v = v / n; return v; } int main() { mut int x = 2; get(x, 0) = 4; return 0; }',
    }
    endpoints = (Endpoint('cpp', 'O3', 'c++23'), Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3'))
    if selection and selection not in successes and selection not in failures and selection not in native_only:
        raise ValueError(f'unknown execution case: {selection}')
    with tempfile.TemporaryDirectory(prefix='gti-llvm-places-') as directory:
        work = Path(directory)
        for name, source_text in {**successes, **failures}.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(source_text, encoding='utf-8')
            results = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f'{name}-{endpoint.backend}-{endpoint.optimization}', source, endpoint, env)
                expected = 70 if name in failures else 0
                code = 'GTI-R0002' if name == 'reference-failure' else 'GTI-R0007'
                if result.error or result.returncode != expected or (expected == 70 and code not in result.stderr):
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            if not results[0] == results[1] == results[2]:
                raise AssertionError(f'{name}: observable drift: {results}')
            print(f'{name}: C++ O3 / LLVM O0 / LLVM O3 passed', flush=True)
        for name, text in native_only.items():
            if selection and selection != name:
                continue
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            for endpoint in (Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3')):
                result = execute(gti, root, work / f'{name}-{endpoint.optimization}', source, endpoint, env)
                if result.error or result.observable != (0, '', ''):
                    raise AssertionError(f'{name} {endpoint}: {result}')
            print(f'{name}: LLVM O0 / O3 source-contract oracle passed (known C++ representation limitation)', flush=True)
    print('LLVM places, selected references, passive arrays and counted text passed')


if __name__ == '__main__':
    main()
