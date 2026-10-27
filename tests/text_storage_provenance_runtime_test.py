#!/usr/bin/env python3
"""Text read from array storage or raw memory runs alike on both backends.

A view copied out of an array of views holds the ownerless text the array
holds, and a C string loaded through a raw pointer is the inert native address
the caller stored there. MIR proves both results ownerless; each program must
build and behave identically on C++ and LLVM at -O0 and -O2.

    python3 tests/text_storage_provenance_runtime_test.py <gti> <repo-root>
"""

from pathlib import Path
import os
import sys
import tempfile

from backend_execution import Endpoint, execute


ARRAY_ELEMENTS = r'''
#include <std/string_view>
std::string_view table[3] = {"zero", "one", "two"};
mut std::string_view kept = "";
std::string_view pick(std::string_view names[3], uint64_t index) { return names[index]; }
std::string_view fromTable(uint64_t index) { return table[index]; }
std::string_view fromLocal(uint64_t index) {
  std::string_view local[2] = {"left", "right"};
  return local[index];
}
std::string_view place(std::string_view text, uint64_t at) {
  mut std::string_view slots[2] = {"first", "second"};
  slots[at] = text;
  return slots[at];
}
int main() {
  std::string_view names[3] = {"ab", "cde", ""};
  kept = pick(names, uint64_t(1));
  std::println(kept);
  std::println(pick(names, uint64_t(0)));
  if (!pick(names, uint64_t(2)).empty() || kept.size() != uint64_t(3)) { return 1; }
  std::println(fromTable(uint64_t(2)));
  std::println(fromLocal(uint64_t(1)));
  std::println(place("placed", uint64_t(1)));
  return pick(names, uint64_t(1)) == "cde" ? 0 : 2;
}
'''

ARRAY_FIELDS_AND_LAMBDAS = r'''
#include <std/string_view>
struct Shelf {
  std::string_view titles[2];
  int count;
  Shelf() : titles({"x", "yz"}), count(2) {}
};
std::string_view fromShelf(Shelf shelf, uint64_t index) { return shelf.titles[index]; }
std::string_view labelled() {
  auto label = []() -> std::string_view { return "label"; };
  std::string_view labels[2] = {label(), "other"};
  return labels[0];
}
int main() {
  std::println(fromShelf(Shelf(), uint64_t(1)));
  std::println(labelled());
  return fromShelf(Shelf(), uint64_t(0)) == "x" ? 0 : 1;
}
'''

ELEMENT_REFERENCES = r'''
#include <std/string_view>
void setTo(mut std::string_view& out, std::string_view value) { out = value; }
std::string_view throughReferences() {
  mut std::string_view words[2] = {"a", "bc"};
  for (mut std::string_view& word : words) { word = "q"; }
  setTo(words[0], "zz");
  std::string_view& last = words[1];
  return last == "q" ? words[0] : "missed";
}
int main() {
  std::println(throughReferences());
  return throughReferences() == "zz" ? 0 : 1;
}
'''

RAW_C_STRINGS = r'''
mut c_string kept = nullptr;
void put(c_string* items, int32_t at, c_string value) { unsafe { items[at] = value; } }
c_string second(c_string* items) { unsafe { c_string* next = items + 1; return *next; } }
c_string at(c_string* items, int32_t index) {
  unsafe {
    c_string* base = items;
    return base[index];
  }
}
int main() {
  mut c_string slots[3] = {nullptr, nullptr, nullptr};
  if (second(&slots[0]) != nullptr) { return 1; }
  put(&slots[0], 1, "b");
  kept = second(&slots[0]);
  if (kept == nullptr || kept != slots[1] || at(&slots[0], 1) != kept) { return 2; }
  if (at(&slots[0], 0) != nullptr || at(&slots[0], 2) != nullptr) { return 3; }
  unsafe { slots[1] = nullptr; }
  return second(&slots[0]) == nullptr ? 0 : 4;
}
'''

CASES = {
    'array-elements': (ARRAY_ELEMENTS, (0, 'cde\nab\ntwo\nright\nplaced\n', '')),
    'array-fields-and-lambdas': (ARRAY_FIELDS_AND_LAMBDAS, (0, 'yz\nlabel\n', '')),
    'element-references': (ELEMENT_REFERENCES, (0, 'zz\n', '')),
    'raw-c-strings': (RAW_C_STRINGS, (0, '', '')),
}

ENDPOINTS = (Endpoint('cpp', 'O0', 'c++20'), Endpoint('cpp', 'O2', 'c++20'),
             Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = dict(os.environ)
    env.setdefault('GTI_STDLIB_PATH', str(root / 'stdlib'))
    with tempfile.TemporaryDirectory(prefix='gti-text-storage-') as directory:
        work = Path(directory)
        for name, (text, expected) in CASES.items():
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            for endpoint in ENDPOINTS:
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.observable != expected:
                    raise AssertionError(f'{name} {endpoint}: {result}')
            print(f'{name}: C++ and LLVM at O0/O2 agree', flush=True)
    print('array-element and raw-memory text returns passed')


if __name__ == '__main__':
    main()
